/******************************************************************************
 * memory.c
 *
 * Code to handle memory-related requests.
 *
 * Copyright (c) 2003-2004, B Dragovic
 * Copyright (c) 2003-2005, K A Fraser
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/perfc.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <xen/paging.h>
#include <xen/iocap.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/errno.h>
#include <xen/tmem.h>
#include <xen/tmem_xen.h>
#include <asm/current.h>
#include <asm/hardirq.h>
#include <asm/p2m.h>
#include <xen/numa.h>
#include <public/memory.h>
#include <xsm/xsm.h>
#include <xen/trace.h>

#ifndef is_domain_direct_mapped
# define is_domain_direct_mapped(d) ((void)(d), 0)
#endif

struct memop_args {
    /* INPUT */
    struct domain *domain;     /* Domain to be affected. */
    XEN_GUEST_HANDLE(xen_pfn_t) extent_list; /* List of extent base addrs. */
    unsigned int nr_extents;   /* Number of extents to allocate or free. */
    unsigned int extent_order; /* Size of each extent. */
    unsigned int memflags;     /* Allocation flags. */

    /* INPUT/OUTPUT */
    unsigned int nr_done;    /* Number of extents processed so far. */
    int          preempted;  /* Was the hypercall preempted? */
};

static void increase_reservation(struct memop_args *a)
{
    struct page_info *page;
    unsigned long i;
    xen_pfn_t mfn;
    struct domain *d = a->domain;

    if ( !guest_handle_is_null(a->extent_list) &&
         !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) )
        return;

    if ( !multipage_allocation_permitted(current->domain, a->extent_order) )
        return;

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }

        page = alloc_domheap_pages(d, a->extent_order, a->memflags);
        if ( unlikely(page == NULL) ) 
        {
            gdprintk(XENLOG_INFO, "Could not allocate order=%d extent: "
                    "id=%d memflags=%x (%ld of %d)\n",
                     a->extent_order, d->domain_id, a->memflags,
                     i, a->nr_extents);
            goto out;
        }

        /* Inform the domain of the new page's machine address. */ 
        if ( !guest_handle_is_null(a->extent_list) )
        {
            mfn = page_to_mfn(page);
            if ( unlikely(__copy_to_guest_offset(a->extent_list, i, &mfn, 1)) )
                goto out;
        }
    }

 out:
    a->nr_done = i;
}

static void populate_physmap(struct memop_args *a)
{
    struct page_info *page;
    unsigned long i, j;
    xen_pfn_t gpfn, mfn;
    struct domain *d = a->domain;

    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) )
        return;

    if ( a->memflags & MEMF_populate_on_demand ? a->extent_order > MAX_ORDER :
         !multipage_allocation_permitted(current->domain, a->extent_order) )
        return;

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }

        if ( unlikely(__copy_from_guest_offset(&gpfn, a->extent_list, i, 1)) )
            goto out;

        if ( a->memflags & MEMF_populate_on_demand )
        {
            if ( guest_physmap_mark_populate_on_demand(d, gpfn,
                                                       a->extent_order) < 0 )
                goto out;
        }
        else
        {
            if ( is_domain_direct_mapped(d) )
            {
                mfn = gpfn;
                if ( !mfn_valid(mfn) )
                {
                    gdprintk(XENLOG_INFO, "Invalid mfn %#"PRI_xen_pfn"\n",
                             mfn);
                    goto out;
                }

                page = mfn_to_page(mfn);
                if ( !get_page(page, d) )
                {
                    gdprintk(XENLOG_INFO,
                             "mfn %#"PRI_xen_pfn" doesn't belong to the"
                             " domain\n", mfn);
                    goto out;
                }
                put_page(page);
            }
            else
                page = alloc_domheap_pages(d, a->extent_order, a->memflags);

            if ( unlikely(page == NULL) ) 
            {
                if ( !opt_tmem || (a->extent_order != 0) )
                    gdprintk(XENLOG_INFO, "Could not allocate order=%d extent:"
                             " id=%d memflags=%x (%ld of %d)\n",
                             a->extent_order, d->domain_id, a->memflags,
                             i, a->nr_extents);
                goto out;
            }

            mfn = page_to_mfn(page);
            guest_physmap_add_page(d, gpfn, mfn, a->extent_order);

            if ( !paging_mode_translate(d) )
            {
                for ( j = 0; j < (1 << a->extent_order); j++ )
                    set_gpfn_from_mfn(mfn + j, gpfn + j);

                /* Inform the domain of the new page's machine address. */ 
                if ( unlikely(__copy_to_guest_offset(a->extent_list, i, &mfn, 1)) )
                    goto out;
            }
        }
    }

out:
    a->nr_done = i;
}

int guest_remove_page(struct domain *d, unsigned long gmfn)
{
    struct page_info *page;
#ifdef CONFIG_X86
    p2m_type_t p2mt;
#endif
    unsigned long mfn;

#ifdef CONFIG_X86
    mfn = mfn_x(get_gfn_query(d, gmfn, &p2mt)); 
    if ( unlikely(p2m_is_paging(p2mt)) )
    {
        guest_physmap_remove_page(d, gmfn, mfn, 0);
        put_gfn(d, gmfn);
        /* If the page hasn't yet been paged out, there is an
         * actual page that needs to be released. */
        if ( p2mt == p2m_ram_paging_out )
        {
            ASSERT(mfn_valid(mfn));
            page = mfn_to_page(mfn);
            if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
                put_page(page);
        }
        p2m_mem_paging_drop_page(d, gmfn, p2mt);
        return 1;
    }
    if ( p2mt == p2m_mmio_direct )
    {
        clear_mmio_p2m_entry(d, gmfn);
        put_gfn(d, gmfn);
        return 1;
    }
#else
    mfn = gmfn_to_mfn(d, gmfn);
#endif
    if ( unlikely(!mfn_valid(mfn)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Domain %u page number %lx invalid\n",
                d->domain_id, gmfn);
        return 0;
    }
            
#ifdef CONFIG_X86
    if ( p2m_is_shared(p2mt) )
    {
        /* Unshare the page, bail out on error. We unshare because 
         * we might be the only one using this shared page, and we
         * need to trigger proper cleanup. Once done, this is 
         * like any other page. */
        if ( mem_sharing_unshare_page(d, gmfn, 0) )
        {
            put_gfn(d, gmfn);
            (void)mem_sharing_notify_enomem(d, gmfn, 0);
            return 0;
        }
        /* Maybe the mfn changed */
        mfn = mfn_x(get_gfn_query_unlocked(d, gmfn, &p2mt));
        ASSERT(!p2m_is_shared(p2mt));
    }
#endif /* CONFIG_X86 */

    page = mfn_to_page(mfn);
    if ( unlikely(!get_page(page, d)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Bad page free for domain %u\n", d->domain_id);
        return 0;
    }

    if ( test_and_clear_bit(_PGT_pinned, &page->u.inuse.type_info) )
        put_page_and_type(page);
            
    if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
        put_page(page);

    guest_physmap_remove_page(d, gmfn, mfn, 0);

    put_page(page);
    put_gfn(d, gmfn);

    return 1;
}

static void decrease_reservation(struct memop_args *a)
{
    unsigned long i, j;
    xen_pfn_t gmfn;

    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) ||
         a->extent_order > MAX_ORDER )
        return;

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }

        if ( unlikely(__copy_from_guest_offset(&gmfn, a->extent_list, i, 1)) )
            goto out;

        if ( tb_init_done )
        {
            struct {
                u64 gfn;
                int d:16,order:16;
            } t;

            t.gfn = gmfn;
            t.d = a->domain->domain_id;
            t.order = a->extent_order;
        
            __trace_var(TRC_MEM_DECREASE_RESERVATION, 0, sizeof(t), &t);
        }

        /* See if populate-on-demand wants to handle this */
        if ( is_hvm_domain(a->domain)
             && p2m_pod_decrease_reservation(a->domain, gmfn, a->extent_order) )
            continue;

        /* With the lack for iommu on some ARM platform, domain with DMA-capable
         * device must retrieve the same pfn when the hypercall
         * populate_physmap is called.
         */
        if ( is_domain_direct_mapped(a->domain) )
            continue;

        for ( j = 0; j < (1 << a->extent_order); j++ )
            if ( !guest_remove_page(a->domain, gmfn + j) )
                goto out;
    }

 out:
    a->nr_done = i;
}

static long memory_exchange(XEN_GUEST_HANDLE_PARAM(xen_memory_exchange_t) arg)
{
    struct xen_memory_exchange exch;
    PAGE_LIST_HEAD(in_chunk_list);
    PAGE_LIST_HEAD(out_chunk_list);
    unsigned long in_chunk_order, out_chunk_order;
    xen_pfn_t     gpfn, gmfn, mfn;
    unsigned long i, j, k = 0; /* gcc ... */
    unsigned int  memflags = 0;
    long          rc = 0;
    struct domain *d;
    struct page_info *page;

    if ( copy_from_guest(&exch, arg, 1) )
        return -EFAULT;

    /* Various sanity checks. */
    if ( (exch.nr_exchanged > exch.in.nr_extents) ||
         /* Input and output domain identifiers match? */
         (exch.in.domid != exch.out.domid) ||
         /* Extent orders are sensible? */
         (exch.in.extent_order > MAX_ORDER) ||
         (exch.out.extent_order > MAX_ORDER) ||
         /* Sizes of input and output lists do not overflow a long? */
         ((~0UL >> exch.in.extent_order) < exch.in.nr_extents) ||
         ((~0UL >> exch.out.extent_order) < exch.out.nr_extents) ||
         /* Sizes of input and output lists match? */
         ((exch.in.nr_extents << exch.in.extent_order) !=
          (exch.out.nr_extents << exch.out.extent_order)) )
    {
        rc = -EINVAL;
        goto fail_early;
    }

    if ( !guest_handle_okay(exch.in.extent_start, exch.in.nr_extents) ||
         !guest_handle_okay(exch.out.extent_start, exch.out.nr_extents) )
    {
        rc = -EFAULT;
        goto fail_early;
    }

    /* Only privileged guests can allocate multi-page contiguous extents. */
    if ( !multipage_allocation_permitted(current->domain,
                                         exch.in.extent_order) ||
         !multipage_allocation_permitted(current->domain,
                                         exch.out.extent_order) )
    {
        rc = -EPERM;
        goto fail_early;
    }

    if ( exch.in.extent_order <= exch.out.extent_order )
    {
        in_chunk_order  = exch.out.extent_order - exch.in.extent_order;
        out_chunk_order = 0;
    }
    else
    {
        in_chunk_order  = 0;
        out_chunk_order = exch.in.extent_order - exch.out.extent_order;
    }

    d = rcu_lock_domain_by_any_id(exch.in.domid);
    if ( d == NULL )
    {
        rc = -ESRCH;
        goto fail_early;
    }

    rc = xsm_memory_exchange(XSM_TARGET, d);
    if ( rc )
    {
        rcu_unlock_domain(d);
        goto fail_early;
    }

    memflags |= MEMF_bits(domain_clamp_alloc_bitsize(
        d,
        XENMEMF_get_address_bits(exch.out.mem_flags) ? :
        (BITS_PER_LONG+PAGE_SHIFT)));
    memflags |= MEMF_node(XENMEMF_get_node(exch.out.mem_flags));

    for ( i = (exch.nr_exchanged >> in_chunk_order);
          i < (exch.in.nr_extents >> in_chunk_order);
          i++ )
    {
        if ( i != (exch.nr_exchanged >> in_chunk_order) &&
             hypercall_preempt_check() )
        {
            exch.nr_exchanged = i << in_chunk_order;
            rcu_unlock_domain(d);
            if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
                return -EFAULT;
            return hypercall_create_continuation(
                __HYPERVISOR_memory_op, "lh", XENMEM_exchange, arg);
        }

        /* Steal a chunk's worth of input pages from the domain. */
        for ( j = 0; j < (1UL << in_chunk_order); j++ )
        {
            if ( unlikely(__copy_from_guest_offset(
                &gmfn, exch.in.extent_start, (i<<in_chunk_order)+j, 1)) )
            {
                rc = -EFAULT;
                goto fail;
            }

            for ( k = 0; k < (1UL << exch.in.extent_order); k++ )
            {
#ifdef CONFIG_X86
                p2m_type_t p2mt;

                /* Shared pages cannot be exchanged */
                mfn = mfn_x(get_gfn_unshare(d, gmfn + k, &p2mt));
                if ( p2m_is_shared(p2mt) )
                {
                    put_gfn(d, gmfn + k);
                    rc = -ENOMEM;
                    goto fail; 
                }
#else /* !CONFIG_X86 */
                mfn = gmfn_to_mfn(d, gmfn + k);
#endif
                if ( unlikely(!mfn_valid(mfn)) )
                {
                    put_gfn(d, gmfn + k);
                    rc = -EINVAL;
                    goto fail;
                }

                page = mfn_to_page(mfn);

                if ( unlikely(steal_page(d, page, MEMF_no_refcount)) )
                {
                    put_gfn(d, gmfn + k);
                    rc = -EINVAL;
                    goto fail;
                }

                page_list_add(page, &in_chunk_list);
                put_gfn(d, gmfn + k);
            }
        }

        /* Allocate a chunk's worth of anonymous output pages. */
        for ( j = 0; j < (1UL << out_chunk_order); j++ )
        {
            page = alloc_domheap_pages(NULL, exch.out.extent_order, memflags);
            if ( unlikely(page == NULL) )
            {
                rc = -ENOMEM;
                goto fail;
            }

            page_list_add(page, &out_chunk_list);
        }

        /*
         * Success! Beyond this point we cannot fail for this chunk.
         */

        /* Destroy final reference to each input page. */
        while ( (page = page_list_remove_head(&in_chunk_list)) )
        {
            unsigned long gfn;

            if ( !test_and_clear_bit(_PGC_allocated, &page->count_info) )
                BUG();
            mfn = page_to_mfn(page);
            gfn = mfn_to_gmfn(d, mfn);
            /* Pages were unshared above */
            BUG_ON(SHARED_M2P(gfn));
            guest_physmap_remove_page(d, gfn, mfn, 0);
            put_page(page);
        }

        /* Assign each output page to the domain. */
        for ( j = 0; (page = page_list_remove_head(&out_chunk_list)); ++j )
        {
            if ( assign_pages(d, page, exch.out.extent_order,
                              MEMF_no_refcount) )
            {
                unsigned long dec_count;
                bool_t drop_dom_ref;

                /*
                 * Pages in in_chunk_list is stolen without
                 * decreasing the tot_pages. If the domain is dying when
                 * assign pages, we need decrease the count. For those pages
                 * that has been assigned, it should be covered by
                 * domain_relinquish_resources().
                 */
                dec_count = (((1UL << exch.in.extent_order) *
                              (1UL << in_chunk_order)) -
                             (j * (1UL << exch.out.extent_order)));

                spin_lock(&d->page_alloc_lock);
                drop_dom_ref = (dec_count &&
                                !domain_adjust_tot_pages(d, -dec_count));
                spin_unlock(&d->page_alloc_lock);

                if ( drop_dom_ref )
                    put_domain(d);

                free_domheap_pages(page, exch.out.extent_order);
                goto dying;
            }

            if ( __copy_from_guest_offset(&gpfn, exch.out.extent_start,
                                          (i << out_chunk_order) + j, 1) )
            {
                rc = -EFAULT;
                continue;
            }

            mfn = page_to_mfn(page);
            guest_physmap_add_page(d, gpfn, mfn, exch.out.extent_order);

            if ( !paging_mode_translate(d) )
            {
                for ( k = 0; k < (1UL << exch.out.extent_order); k++ )
                    set_gpfn_from_mfn(mfn + k, gpfn + k);
                if ( __copy_to_guest_offset(exch.out.extent_start,
                                            (i << out_chunk_order) + j,
                                            &mfn, 1) )
                    rc = -EFAULT;
            }
        }
        BUG_ON( !(d->is_dying) && (j != (1UL << out_chunk_order)) );
    }

    exch.nr_exchanged = exch.in.nr_extents;
    if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
        rc = -EFAULT;
    rcu_unlock_domain(d);
    return rc;

    /*
     * Failed a chunk! Free any partial chunk work. Tell caller how many
     * chunks succeeded.
     */
 fail:
    /* Reassign any input pages we managed to steal. */
    while ( (page = page_list_remove_head(&in_chunk_list)) )
    {
        put_gfn(d, gmfn + k--);
        if ( assign_pages(d, page, 0, MEMF_no_refcount) )
            BUG();
    }

 dying:
    rcu_unlock_domain(d);
    /* Free any output pages we managed to allocate. */
    while ( (page = page_list_remove_head(&out_chunk_list)) )
        free_domheap_pages(page, exch.out.extent_order);

    exch.nr_exchanged = i << in_chunk_order;

 fail_early:
    if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
        rc = -EFAULT;
    return rc;
}

static int xenmem_add_to_physmap(struct domain *d,
                                 struct xen_add_to_physmap *xatp,
                                 unsigned int start)
{
    unsigned int done = 0;
    long rc = 0;

    if ( xatp->space != XENMAPSPACE_gmfn_range )
        return xenmem_add_to_physmap_one(d, xatp->space, DOMID_INVALID,
                                         xatp->idx, xatp->gpfn);

    if ( xatp->size < start )
        return -EILSEQ;

    xatp->idx += start;
    xatp->gpfn += start;
    xatp->size -= start;

#ifdef HAS_PASSTHROUGH
    if ( need_iommu(d) )
        this_cpu(iommu_dont_flush_iotlb) = 1;
#endif

    while ( xatp->size > done )
    {
        rc = xenmem_add_to_physmap_one(d, xatp->space, DOMID_INVALID,
                                       xatp->idx, xatp->gpfn);
        if ( rc < 0 )
            break;

        xatp->idx++;
        xatp->gpfn++;

        /* Check for continuation if it's not the last iteration. */
        if ( xatp->size > ++done && hypercall_preempt_check() )
        {
            rc = start + done;
            break;
        }
    }

#ifdef HAS_PASSTHROUGH
    if ( need_iommu(d) )
    {
        this_cpu(iommu_dont_flush_iotlb) = 0;
        iommu_iotlb_flush(d, xatp->idx - done, done);
        iommu_iotlb_flush(d, xatp->gpfn - done, done);
    }
#endif

    return rc;
}

static int xenmem_add_to_physmap_batch(struct domain *d,
                                       struct xen_add_to_physmap_batch *xatpb,
                                       unsigned int start)
{
    unsigned int done = 0;
    int rc;

    if ( xatpb->size < start )
        return -EILSEQ;

    guest_handle_add_offset(xatpb->idxs, start);
    guest_handle_add_offset(xatpb->gpfns, start);
    guest_handle_add_offset(xatpb->errs, start);
    xatpb->size -= start;

    while ( xatpb->size > done )
    {
        xen_ulong_t idx;
        xen_pfn_t gpfn;

        if ( unlikely(__copy_from_guest_offset(&idx, xatpb->idxs, 0, 1)) )
        {
            rc = -EFAULT;
            goto out;
        }

        if ( unlikely(__copy_from_guest_offset(&gpfn, xatpb->gpfns, 0, 1)) )
        {
            rc = -EFAULT;
            goto out;
        }

        rc = xenmem_add_to_physmap_one(d, xatpb->space,
                                       xatpb->foreign_domid,
                                       idx, gpfn);

        if ( unlikely(__copy_to_guest_offset(xatpb->errs, 0, &rc, 1)) )
        {
            rc = -EFAULT;
            goto out;
        }

        guest_handle_add_offset(xatpb->idxs, 1);
        guest_handle_add_offset(xatpb->gpfns, 1);
        guest_handle_add_offset(xatpb->errs, 1);

        /* Check for continuation if it's not the last iteration. */
        if ( xatpb->size > ++done && hypercall_preempt_check() )
        {
            rc = start + done;
            goto out;
        }
    }

    rc = 0;

out:
    return rc;
}

long do_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    struct domain *d;
    long rc;
    unsigned int address_bits;
    struct xen_memory_reservation reservation;
    struct memop_args args;
    domid_t domid;
    unsigned long start_extent = cmd >> MEMOP_EXTENT_SHIFT;
    int op = cmd & MEMOP_CMD_MASK;

    switch ( op )
    {
    case XENMEM_increase_reservation:
    case XENMEM_decrease_reservation:
    case XENMEM_populate_physmap:
        if ( copy_from_guest(&reservation, arg, 1) )
            return start_extent;

        /* Is size too large for us to encode a continuation? */
        if ( reservation.nr_extents > (UINT_MAX >> MEMOP_EXTENT_SHIFT) )
            return start_extent;

        if ( unlikely(start_extent >= reservation.nr_extents) )
            return start_extent;

        args.extent_list  = reservation.extent_start;
        args.nr_extents   = reservation.nr_extents;
        args.extent_order = reservation.extent_order;
        args.nr_done      = start_extent;
        args.preempted    = 0;
        args.memflags     = 0;

        address_bits = XENMEMF_get_address_bits(reservation.mem_flags);
        if ( (address_bits != 0) &&
             (address_bits < (get_order_from_pages(max_page) + PAGE_SHIFT)) )
        {
            if ( address_bits <= PAGE_SHIFT )
                return start_extent;
            args.memflags = MEMF_bits(address_bits);
        }

        args.memflags |= MEMF_node(XENMEMF_get_node(reservation.mem_flags));
        if ( reservation.mem_flags & XENMEMF_exact_node_request )
            args.memflags |= MEMF_exact_node;

        if ( op == XENMEM_populate_physmap
             && (reservation.mem_flags & XENMEMF_populate_on_demand) )
            args.memflags |= MEMF_populate_on_demand;

        d = rcu_lock_domain_by_any_id(reservation.domid);
        if ( d == NULL )
            return start_extent;
        args.domain = d;

        rc = xsm_memory_adjust_reservation(XSM_TARGET, current->domain, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        switch ( op )
        {
        case XENMEM_increase_reservation:
            increase_reservation(&args);
            break;
        case XENMEM_decrease_reservation:
            decrease_reservation(&args);
            break;
        default: /* XENMEM_populate_physmap */
            populate_physmap(&args);
            break;
        }

        rcu_unlock_domain(d);

        rc = args.nr_done;

        if ( args.preempted )
            return hypercall_create_continuation(
                __HYPERVISOR_memory_op, "lh",
                op | (rc << MEMOP_EXTENT_SHIFT), arg);

        break;

    case XENMEM_exchange:
        rc = memory_exchange(guest_handle_cast(arg, xen_memory_exchange_t));
        break;

    case XENMEM_maximum_ram_page:
        rc = max_page;
        break;

    case XENMEM_current_reservation:
    case XENMEM_maximum_reservation:
    case XENMEM_maximum_gpfn:
        if ( copy_from_guest(&domid, arg, 1) )
            return -EFAULT;

        d = rcu_lock_domain_by_any_id(domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_memory_stat_reservation(XSM_TARGET, current->domain, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        switch ( op )
        {
        case XENMEM_current_reservation:
            rc = d->tot_pages;
            break;
        case XENMEM_maximum_reservation:
            rc = d->max_pages;
            break;
        default:
            ASSERT(op == XENMEM_maximum_gpfn);
            rc = domain_get_maximum_gpfn(d);
            break;
        }

        rcu_unlock_domain(d);

        break;

    case XENMEM_add_to_physmap:
    {
        struct xen_add_to_physmap xatp;

        BUILD_BUG_ON((typeof(xatp.size))-1 > (UINT_MAX >> MEMOP_EXTENT_SHIFT));

        /* Check for malicious or buggy input. */
        if ( start_extent != (typeof(xatp.size))start_extent )
            return -EDOM;

        if ( copy_from_guest(&xatp, arg, 1) )
            return -EFAULT;

        /* Foreign mapping is only possible via add_to_physmap_batch. */
        if ( xatp.space == XENMAPSPACE_gmfn_foreign )
            return -ENOSYS;

        d = rcu_lock_domain_by_any_id(xatp.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_add_to_physmap(XSM_TARGET, current->domain, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        rc = xenmem_add_to_physmap(d, &xatp, start_extent);

        rcu_unlock_domain(d);

        if ( xatp.space == XENMAPSPACE_gmfn_range && rc > 0 )
            rc = hypercall_create_continuation(
                     __HYPERVISOR_memory_op, "lh",
                     op | (rc << MEMOP_EXTENT_SHIFT), arg);

        return rc;
    }

    case XENMEM_add_to_physmap_batch:
    {
        struct xen_add_to_physmap_batch xatpb;
        struct domain *d;

        BUILD_BUG_ON((typeof(xatpb.size))-1 >
                     (UINT_MAX >> MEMOP_EXTENT_SHIFT));

        /* Check for malicious or buggy input. */
        if ( start_extent != (typeof(xatpb.size))start_extent )
            return -EDOM;

        if ( copy_from_guest(&xatpb, arg, 1) ||
             !guest_handle_okay(xatpb.idxs, xatpb.size) ||
             !guest_handle_okay(xatpb.gpfns, xatpb.size) ||
             !guest_handle_okay(xatpb.errs, xatpb.size) )
            return -EFAULT;

        /* This mapspace is unsupported for this hypercall. */
        if ( xatpb.space == XENMAPSPACE_gmfn_range )
            return -EOPNOTSUPP;

        d = rcu_lock_domain_by_any_id(xatpb.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_add_to_physmap(XSM_TARGET, current->domain, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        rc = xenmem_add_to_physmap_batch(d, &xatpb, start_extent);

        rcu_unlock_domain(d);

        if ( rc > 0 )
            rc = hypercall_create_continuation(
                    __HYPERVISOR_memory_op, "lh",
                    op | (rc << MEMOP_EXTENT_SHIFT), arg);

        return rc;
    }

    case XENMEM_remove_from_physmap:
    {
        struct xen_remove_from_physmap xrfp;
        struct page_info *page;
        struct domain *d;

        if ( copy_from_guest(&xrfp, arg, 1) )
            return -EFAULT;

        d = rcu_lock_domain_by_any_id(xrfp.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_remove_from_physmap(XSM_TARGET, current->domain, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        page = get_page_from_gfn(d, xrfp.gpfn, NULL, P2M_ALLOC);
        if ( page )
        {
            guest_physmap_remove_page(d, xrfp.gpfn, page_to_mfn(page), 0);
            put_page(page);
        }
        else
            rc = -ENOENT;

        rcu_unlock_domain(d);

        break;
    }

    case XENMEM_claim_pages:
        if ( copy_from_guest(&reservation, arg, 1) )
            return -EFAULT;

        if ( !guest_handle_is_null(reservation.extent_start) )
            return -EINVAL;

        if ( reservation.extent_order != 0 )
            return -EINVAL;

        if ( reservation.mem_flags != 0 )
            return -EINVAL;

        d = rcu_lock_domain_by_id(reservation.domid);
        if ( d == NULL )
            return -EINVAL;

        rc = xsm_claim_pages(XSM_PRIV, d);

        if ( !rc )
            rc = domain_set_outstanding_pages(d, reservation.nr_extents);

        rcu_unlock_domain(d);

        break;

    default:
        rc = arch_memory_op(cmd, arg);
        break;
    }

    return rc;
}

#ifdef BIGOS_MEMORY_MOVE
/*
 * Contains the gfn which *may be* write protected while it is copied to
 * another location.
 * This information should be used by the VMEXIT PGFAULT-like handler to
 * perform a short-term wait until the gfn has been copied.
 */
static DEFINE_SPINLOCK(memory_moved_spinlock);
static DEFINE_SPINLOCK(memory_moved_waiter);
static unsigned long  memory_moved_gfn = INVALID_GFN;
static struct domain *memory_moved_domain = NULL;


int is_memory_moved_gfn(struct domain *d, unsigned long gfn, int wait)
{
    spin_lock(&memory_moved_spinlock);

    if ( d != memory_moved_domain )
        goto out;
    if ( gfn != memory_moved_gfn )
        goto out;

    spin_unlock(&memory_moved_spinlock);

    /*
     * When set_memory_moved_gfn() is called, it lock memory_moved_waiter.
     * So the spin_lock() here blocks until clear_memory_moved_gfn()
     * is called and unlock the waiter.
     * Then immediately unlock the waiter to allow other blocked cpus
     * to continue.
     */

    if ( wait ) {
        spin_lock(&memory_moved_waiter);
        spin_unlock(&memory_moved_waiter);
    }

    return 1;
out:
    spin_unlock(&memory_moved_spinlock);
    return 0;
}

void set_memory_moved_gfn(struct domain *d, unsigned long gfn)
{
    ASSERT(memory_moved_gfn == INVALID_GFN);
    ASSERT(memory_moved_domain == NULL);

    spin_lock(&memory_moved_spinlock);

    memory_moved_gfn = gfn;
    memory_moved_domain = d;

    spin_lock(&memory_moved_waiter);

    spin_unlock(&memory_moved_spinlock);
}

void clear_memory_moved_gfn(void)
{
    ASSERT(memory_moved_gfn != INVALID_GFN);
    ASSERT(memory_moved_domain != NULL);

    spin_lock(&memory_moved_spinlock);

    memory_moved_gfn = INVALID_GFN;
    memory_moved_domain = NULL;

    spin_unlock(&memory_moved_waiter);

    spin_unlock(&memory_moved_spinlock);
}


/*
 * Try to destroy all links of a given page for a specified domain, leaving it
 * with only one reference to its counter and not assigned anymore to the
 * domain.
 * In case of success, returns the associated page_info and take a reference
 * on the gfn.
 * Otherwise, retur NULL and no reference is taken.
 */
static struct page_info*__memory_move_steal(struct domain *d,unsigned long gfn)
{
    struct page_info *page = NULL;
    unsigned long mfn;
#ifdef CONFIG_X86
    p2m_type_t p2mt;

    /* Shared pages cannot be moved */
    mfn = mfn_x(get_gfn_unshare(d, gfn, &p2mt));    /* get the gfn */
    if ( p2m_is_shared(p2mt) )
        goto err;
#else /* !CONFIG_X86 */
    mfn = gmfn_to_mfn(d, gfn);                      /* get the gfn */
#endif

    if ( unlikely(!mfn_valid(mfn)) )
        goto err;

    page = mfn_to_page(mfn);
    if ( unlikely(steal_page(d, page, MEMF_no_refcount)) )
        goto err;

    return page;                                    /* ref taken on success */
 err:
    put_gfn(d, gfn);                                /* put the gfn */
    return NULL;                                    /* ref neutral on fail */
}

/*
 * Replace, for a given gfn of a given domain, the old associated mfn by a
 * new one, then update all the TLBs.
 * The data are moved transparently from the old mfn to the new one so there
 * is no functional effect on the domain.
 * If the new mfn cannot be assigned to the domain, nothing happens and the
 * function returns -1.
 * In case of success, the function returns 0.
 */
static int __memory_move_replace(struct domain *d, unsigned long gfn,
                                 struct page_info *old, struct page_info *new)
{
    unsigned long old_mfn = page_to_mfn(old), new_mfn = page_to_mfn(new);
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    ASSERT(gfn == mfn_to_gmfn(d, old_mfn));
    ASSERT(old->count_info & _PGC_allocated);
    ASSERT(new->count_info == 0);
    ASSERT(mfn_valid(new_mfn));
    ASSERT(mfn_valid(old_mfn));
    ASSERT(!SHARED_M2P(gfn));

    if ( assign_pages(d, new, 0, MEMF_no_refcount) )
        return -1;

    set_memory_moved_gfn(d, gfn);                  /* gfn is fault protected */

    /*
     * First step, remove the write access on the old mfn, and flush the TLBs
     * for the appropriate entry.
     * NB: be carefull, the "p2m_access_rx" can be changed to the p2m default
     *     access type (p2m_access_rwx) for random reasons.
     *     We use p2m_ram_ro which is a type indicated to silently drop writes
     *     on the page, and intercept them in page fault handler.
     */

    p2m->set_entry(p2m, gfn, _mfn(old_mfn), 0, p2m_ram_ro, p2m_access_rx);

    /*
     * Here, the content of the page can be read but not modified, so we can
     * safely perform the copy to the new mfn.
     */

    copy_domain_page(new_mfn, old_mfn);

    /*
     * Now we can replace the old mfn by the new one, which has write access,
     * and then flush the TLBs again for the appropriate entry.
     */

    guest_physmap_add_page(d, gfn, new_mfn, 0);

    clear_memory_moved_gfn();          /* gfn is not fault protected anymore */

    put_page(old);             /* release the last reference on the old page */

    if ( !paging_mode_translate(d) )
        set_gpfn_from_mfn(new_mfn, gfn);

    return 0;
}

unsigned long memory_move(struct domain *d, unsigned long gfn,
                          unsigned long node)
{
    unsigned int memflags;
    struct page_info *old = NULL;
    struct page_info *new = NULL;

    ASSERT(node < MAX_NUMNODES);

    memflags = domain_clamp_alloc_bitsize(d, BITS_PER_LONG + PAGE_SHIFT);
    memflags = MEMF_bits(memflags);
    memflags = memflags | MEMF_node(node) | MEMF_exact_node;

    /*
     * TODO: it may be necessary to lock the p2m of the domain during the
     * entire execution of this function.
     * The problem is the guest_physmap_add_page() invocation in
     * __memory_move_replace() already takes a (non-reentrant) lock.
     * The solution could be to add a parameter "assume already locked" to this
     * function and provide a compatibility interface with this parameter to 0.
     */

    /* In success, deassign the old mfn from the domain */
    old = __memory_move_steal(d, gfn);                        /* get the gfn */
    if ( unlikely(old == NULL) )                         /* unless it failed */
        goto fail_gfn;

    new = alloc_domheap_pages(NULL, 0, memflags);
    if ( unlikely(new == NULL) )
        goto fail_old;

    if ( __memory_move_replace(d, gfn, old, new) )
        goto fail_new;

    put_gfn(d, gfn);                                          /* put the gfn */

    return page_to_mfn(new);
 fail_new:
    free_domheap_pages(new, 0);
 fail_old:
    put_gfn(d, gfn);                                          /* put the gfn */
    /* Now reassign the old mfn to the domain */
    if ( assign_pages(d, old, 0, MEMF_no_refcount) )
        BUG();
 fail_gfn:
    return INVALID_MFN;
}
#endif /* BIGOS_MEMORY_MOVE */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
