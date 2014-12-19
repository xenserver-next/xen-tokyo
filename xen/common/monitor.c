#include <asm/guest_access.h>
#include <asm/ibs.h>
#include <asm/page.h>
#include <asm/paging.h>
#include <asm/pebs.h>
#include <asm/system.h>
#include <xen/cpumask.h>
#include <xen/config.h>
#include <xen/hotlist.h>
#include <xen/lib.h>
#include <xen/migration.h>
#include <xen/monitor.h>
#include <xen/percpu.h>


struct migration_query
{
    unsigned long   mfn;
    unsigned int    node;
    unsigned long   gfn;
    struct domain  *domain;
    unsigned int    tries;
};


static int monitoring_started = 0;            /* is the monitoring running ? */

static DEFINE_PER_CPU(unsigned long, migration_engine_owner);
#define OWNER_NONE      0
#define OWNER_SAMPLER   1
#define OWNER_DECIDER   2

static struct migration_query *migration_queue;


static unsigned long  monitor_tracked = BIGOS_MONITOR_TRACKED;
static unsigned long  monitor_candidate = BIGOS_MONITOR_CANDIDATE;
static unsigned long  monitor_enqueued = BIGOS_MONITOR_ENQUEUED;
static unsigned int   monitor_enter = BIGOS_MONITOR_ENTER;
static unsigned int   monitor_increment = BIGOS_MONITOR_INCREMENT;
static unsigned int   monitor_decrement = BIGOS_MONITOR_DECREMENT;
static unsigned int   monitor_maximum = BIGOS_MONITOR_MAXIMUM;
static unsigned int   monitor_min_node_score = BIGOS_MONITOR_MIN_NODE_SCORE;
static unsigned int   monitor_min_node_rate = BIGOS_MONITOR_MIN_NODE_RATE;
static unsigned char  monitor_flush_after_refill = BIGOS_MONITOR_FLUSH;
static unsigned int   monitor_maxtries = BIGOS_MONITOR_MAXTRIES;


#ifdef BIGOS_STATS

static s_time_t         stats_start = 0;
static s_time_t         stats_end = 0;

static DEFINE_PER_CPU(s_time_t, time_counter_0);
static DEFINE_PER_CPU(s_time_t, time_counter_1);
static s_time_t         time_counter_2;

static DEFINE_PER_CPU(unsigned long, sampling_count);  /* IBS/PEBS count    */
static DEFINE_PER_CPU(s_time_t, sampling_total_time);  /* IBS/PEBS total ns */
static DEFINE_PER_CPU(s_time_t, sampling_accounting_time);    /* hotlist ns */
static DEFINE_PER_CPU(s_time_t, sampling_probing_time);/* info gathering ns */

static s_time_t         decision_total_time = 0;       /* planning time  ns */
static s_time_t         listwalk_total_time = 0;       /* popping time   ns */
static s_time_t         migration_total_time = 0;      /* migration time ns */

static unsigned long    decision_count = 0;        /* # decision process */
static unsigned long    migration_planned = 0;     /* # page in info_buffer */
static unsigned long    migration_tries = 0;       /* # memory_move call */
static unsigned long    migration_succeed = 0;     /* # memory_move return 0 */
static unsigned long    migration_aborted = 0;     /* # maxtries cancel */

static void reset_stats(void)
{
    int cpu;

    for_each_online_cpu ( cpu )
    {
        per_cpu(sampling_total_time, cpu) = 0;
        per_cpu(sampling_accounting_time, cpu) = 0;
        per_cpu(sampling_probing_time, cpu) = 0;
        per_cpu(sampling_count, cpu) = 0;
    }
    decision_count = 0;
    decision_total_time = 0;
    listwalk_total_time = 0;
    migration_total_time = 0;
    migration_planned = 0;
    migration_tries = 0;
    migration_succeed = 0;
    migration_aborted = 0;
    stats_start = 0;
    stats_end = 0;
}

#define stats_start()        stats_start = NOW()
#define stats_end()          stats_end = NOW()

#define stats_start_sampling()                  \
    this_cpu(time_counter_0) = NOW(), this_cpu(sampling_count)++
#define stats_stop_sampling()                                           \
    this_cpu(sampling_total_time) += (NOW() - this_cpu(time_counter_0))

#define stats_start_accounting()   this_cpu(time_counter_1) = NOW()
#define stats_stop_accounting()                                         \
    this_cpu(sampling_accounting_time) += (NOW() - this_cpu(time_counter_1))

#define stats_start_probing()      this_cpu(time_counter_1) = NOW()
#define stats_stop_probing()                                            \
    this_cpu(sampling_probing_time) += (NOW() - this_cpu(time_counter_1))

#define stats_start_decision()     \
    time_counter_2 = NOW(), decision_count++
#define stats_stop_decision()                       \
    decision_total_time += (NOW() - time_counter_2)

#define stats_start_migration()    time_counter_2 = NOW()
#define stats_stop_migration()                          \
    migration_total_time += (NOW() - time_counter_2)


#define stats_account_migration_plan()          \
    migration_planned++

#define stats_account_migration_abort()          \
    migration_aborted++

#define stats_account_migration_try(ret)            \
    migration_tries++, migration_succeed += (!(ret))


#define MIN_MAX_AVG(percpu, min, max, avg)          \
    {                                               \
        unsigned int ____cpu;                       \
        unsigned long ____count = 0;                \
        min = -1; max = 0; avg = 0;                 \
        for_each_online_cpu ( ____cpu )             \
        {                                           \
            ____count++;                            \
            if ( per_cpu(percpu, ____cpu) < min )   \
                min = per_cpu(percpu, ____cpu);     \
            if ( per_cpu(percpu, ____cpu) > max )   \
                max = per_cpu(percpu, ____cpu);     \
            avg += per_cpu(percpu, ____cpu);        \
        }                                           \
        avg /= ____count;                           \
    }

static void display_stats(void)
{
    unsigned long min, max, avg;

    printk("   ***   BIGOS STATISTICS   ***   \n");
    printk("statistics over %lu nanoseconds\n", stats_end - stats_start);
    printk("\n");

    MIN_MAX_AVG(sampling_count, min, max, avg);
    printk("sampling total count         %lu/%lu/%lu\n", min, max, avg);
    MIN_MAX_AVG(sampling_total_time, min, max, avg);
    printk("sampling total time          %lu/%lu/%lu ns\n", min, max, avg);
    MIN_MAX_AVG(sampling_accounting_time, min, max, avg);
    printk("sampling accounting time     %lu/%lu/%lu ns\n", min, max, avg);
    MIN_MAX_AVG(sampling_probing_time, min, max, avg);
    printk("sampling probing time        %lu/%lu/%lu ns\n", min, max, avg);
    printk("\n");
    printk("decision total count         %lu\n", decision_count);
    printk("decision total time          %lu ns\n", decision_total_time);
    printk("\n");
    printk("migration total time         %lu ns\n", migration_total_time);
    printk("migration planned            %lu\n", migration_planned);
    printk("migration tries              %lu\n", migration_tries);
    printk("migration succeed            %lu\n", migration_succeed);
    printk("migration aborted            %lu\n", migration_aborted);
    printk("\n");

    MIN_MAX_AVG(sampling_total_time, min, max, avg);
    printk("total overhead               %lu%%\n",
           ((max + decision_total_time + migration_total_time)
            * 100) / (stats_end - stats_start + 1));
}

#else

#define reset_stats()                      {}
#define sstats_tart()                      {}
#define stats_end()                        {}
#define stats_start_sampling()             {}
#define stats_stop_sampling()              {}
#define stats_start_accounting()           {}
#define stats_stop_accounting()            {}
#define stats_start_probing()              {}
#define stats_stop_probing()               {}
#define stats_start_decision()             {}
#define stats_stop_decision()              {}
#define stats_start_migration()            {}
#define stats_stop_migration()             {}
#define stats_account_migration_plan()     {}
#define stats_account_migration_abort()    {}
#define stats_account_migration_try(ret)   {}
#define display_stats()                    {}

#endif


static int alloc_migration_queue(void)
{
	int ret = 0;
	unsigned long order, size = monitor_enqueued;

    order = get_order_from_bytes(size * sizeof(struct migration_query));
	migration_queue = alloc_xenheap_pages(order, 0);

	if ( migration_queue == NULL )
		ret = -1;
	return ret;
}

static void init_migration_queue(void)
{
    unsigned long i;

    for (i=0; i<monitor_enqueued; i++)
        migration_queue[i].mfn = INVALID_MFN;
}

static void free_migration_queue(void)
{
	unsigned long order, size = monitor_enqueued;

    if ( migration_queue == NULL )
        return;
	order = get_order_from_bytes(size * sizeof(struct hotlist_entry));

	free_xenheap_pages(migration_queue, order);
    migration_queue = NULL;
}




static void fill_migration_queue(struct migration_buffer *buffer)
{
    unsigned long i, j, slot;

    for (i=0; i<buffer->size; i++)
    {
        slot = monitor_enqueued;

        for (j=0; j<monitor_enqueued; j++)
            if ( migration_queue[j].mfn == buffer->migrations[i].pgid )
                break;
            else if ( migration_queue[j].mfn == INVALID_MFN )
                slot = j;

        if ( j != monitor_enqueued )         /* entry already present */
            continue;

        stats_account_migration_plan();

        if ( slot == monitor_enqueued )      /* no more empty slot */
            break;

        migration_queue[slot].mfn = buffer->migrations[i].pgid;
        migration_queue[slot].node = buffer->migrations[i].node;
        migration_queue[slot].gfn = INVALID_GFN;
        migration_queue[slot].domain = NULL;
        migration_queue[slot].tries = 0;
    }
}

static void drain_migration_queue(void)
{
    unsigned long i;
    unsigned long nid;
    int ret;

    for (i=0; i<monitor_enqueued; i++)
    {
        if ( migration_queue[i].mfn == INVALID_MFN )
            continue;

        nid = phys_to_nid(migration_queue[i].mfn << PAGE_SHIFT);
        if ( migration_queue[i].node == nid )
        {
            register_page_moved(migration_queue[i].mfn);
            migration_queue[i].mfn = INVALID_MFN;
            continue;
        }

        if ( migration_queue[i].gfn == INVALID_GFN )
        {
            if ( ++migration_queue[i].tries >= monitor_maxtries )
            {
                migration_queue[i].mfn = INVALID_MFN;
                stats_account_migration_abort();
            }
            continue;
        }

        stats_start_migration();
        ret = memory_move(migration_queue[i].domain, migration_queue[i].gfn,
                          migration_queue[i].node);
        stats_stop_migration();

        stats_account_migration_try(ret);
        register_page_moved(migration_queue[i].mfn);
        migration_queue[i].mfn = INVALID_MFN;
    }
}

int decide_migration(void)
{
    int cpu;
    struct migration_buffer *buffer;

    if ( !monitoring_started )
        return -1;

    for_each_online_cpu ( cpu )
        while ( cmpxchg(&per_cpu(migration_engine_owner, cpu), OWNER_NONE,
                        OWNER_DECIDER) != OWNER_NONE )
            ;

    drain_migration_queue();

    stats_start_decision();
    buffer = refill_migration_buffer();
    fill_migration_queue(buffer);
    stats_stop_decision();

    for_each_online_cpu ( cpu )
        cmpxchg(&per_cpu(migration_engine_owner, cpu), OWNER_DECIDER,
                OWNER_NONE);
    return 0;
}


/* static void pebs_nmi_handler(struct pebs_record *record, int cpu) */
/* { */
/*     printk("CPU[%d] <= 0x%lx\n", cpu, record->data_linear_address); */
/* } */

static int enable_monitoring_pebs(void)
{
    /* int ret; */

    /* ret = pebs_acquire(); */
    /* if ( ret ) */
    /*     return ret; */

    /* pebs_setevent(PEBS_MUOPS | PEBS_MUOPS_ALLLD); */
    /* pebs_setrate(0x10000); */
    /* pebs_sethandler(pebs_nmi_handler); */
    /* pebs_enable(); */
    printk("PEBS useless in virtualization context !\n");
    return 0;
}

static void disable_monitoring_pebs(void)
{
    struct migration_buffer *buffer;
    unsigned long i;

    alloc_migration_engine(4, 6, 4);
    init_migration_engine();
    param_migration_engine(75, 8, 0);

    refill_migration_buffer();

    register_page_access_cpu(42, 0);
    register_page_access_cpu(23, 0);
    register_page_access_cpu(42, 0);
    register_page_access_cpu(42, 0);

    register_page_access_cpu(18, 1);

    register_page_access_cpu(17, 2);
    register_page_access_cpu(42, 2);

    register_page_access_cpu(18, 3);
    register_page_access_cpu(18, 3);
    register_page_access_cpu(18, 3);
    register_page_access_cpu(23, 3);
    register_page_access_cpu(23, 3);
    register_page_access_cpu(23, 3);

    buffer = refill_migration_buffer();
    for (i=0; i<buffer->size; i++)
        printk("migration of %lu to %u\n", buffer->migrations[i].pgid,
               buffer->migrations[i].node);
    fill_migration_queue(buffer);

    free_migration_engine();

    /* pebs_disable(); */
    /* pebs_release(); */
}

static void ibs_nmi_handler(struct ibs_record *record)
{
    unsigned long i, vaddr, gfn, ogfn, mfn;
    uint32_t pfec;

    if ( cmpxchg(&this_cpu(migration_engine_owner), OWNER_NONE,
                 OWNER_SAMPLER) != OWNER_NONE )
        return;

    stats_start_sampling();

    if ( !(record->record_mode & IBS_RECORD_MODE_OP) )
        goto out;
    if ( !(record->record_mode & IBS_RECORD_MODE_DPA) )
        goto out;
    if ( current->domain->domain_id >= DOMID_FIRST_RESERVED )
        goto out;
    if ( current->domain->guest_type != guest_type_hvm )
        goto out;

    vaddr = record->data_linear_address;
    mfn = record->data_physical_address >> PAGE_SHIFT;

    for (i=0; i<monitor_enqueued; i++)
    {
        if ( migration_queue[i].mfn != mfn )
            continue;

        local_irq_enable();
        pfec = PFEC_page_present;

        stats_start_probing();
        gfn = try_paging_gva_to_gfn(current, vaddr, &pfec);
        stats_stop_probing();

        local_irq_disable();

        ogfn = INVALID_GFN;

        if ( cmpxchg(&migration_queue[i].gfn, ogfn, gfn) != ogfn )
            continue;
        migration_queue[i].domain = current->domain;
    }

    stats_start_accounting();
    register_page_access(mfn);
    stats_stop_accounting();

out:
    stats_stop_sampling();
    cmpxchg(&this_cpu(migration_engine_owner), OWNER_SAMPLER, OWNER_NONE);
}

static int enable_monitoring_ibs(void)
{
    int ret;

    ret = ibs_acquire();
    if ( ret )
        return ret;

    ibs_setevent(IBS_EVENT_OP);
    ibs_setrate(0x1000000);
    ibs_sethandler(ibs_nmi_handler);
    ibs_enable();

    return 0;
}

static void disable_monitoring_ibs(void)
{
    ibs_disable();
    ibs_release();
}


int monitor_migration_settracked(unsigned long tracked)
{
    int restart = monitoring_started;

    stop_monitoring();
    monitor_tracked = tracked;

    if ( restart )
        return start_monitoring();
    return 0;
}

int monitor_migration_setcandidate(unsigned long candidate)
{
    int restart = monitoring_started;

    stop_monitoring();
    monitor_candidate = candidate;

    if ( restart )
        return start_monitoring();
    return 0;
}

int monitor_migration_setenqueued(unsigned long enqueued)
{
    int restart = monitoring_started;

    stop_monitoring();
    monitor_enqueued = enqueued;

    if ( restart )
        return start_monitoring();
    return 0;
}

int monitor_migration_setscores(unsigned int enter, unsigned int increment,
                                unsigned int decrement, unsigned int maximum)
{
    monitor_enter = enter;
    monitor_increment = increment;
    monitor_decrement = decrement;
    monitor_maximum = maximum;

    if ( monitoring_started )
        param_migration_lists(enter, increment, decrement, maximum);

    return 0;
}

int monitor_migration_setcriterias(unsigned int min_node_score,
                                   unsigned int min_node_rate,
                                   unsigned char flush_after_refill)
{
    monitor_min_node_score = min_node_score;
    monitor_min_node_rate = min_node_rate;
    monitor_flush_after_refill = flush_after_refill;

    if ( monitoring_started )
        param_migration_engine(min_node_score, min_node_rate,
                               flush_after_refill);

    return 0;
}

int monitor_migration_setrules(unsigned int maxtries)
{
    monitor_maxtries = maxtries;
    return 0;
}


int start_monitoring(void)
{
    if ( monitoring_started )
        return -1;
    reset_stats();

    if ( alloc_migration_queue() != 0 )
        goto err;
    if ( alloc_migration_engine(monitor_tracked, monitor_candidate,
                                monitor_enqueued) != 0 )
        goto err_queue;

    init_migration_queue();
    init_migration_engine();

    param_migration_lists(monitor_enter, monitor_increment,
                          monitor_decrement, monitor_maximum);
    param_migration_engine(monitor_min_node_rate, monitor_min_node_score,
                           monitor_flush_after_refill);

    if ( ibs_capable() && enable_monitoring_ibs() == 0 )
        goto out;
    if ( pebs_capable() && enable_monitoring_pebs() == 0 )
        goto out;
    goto err_engine;

out:
    monitoring_started = 1;
    stats_start();
    return 0;
err_engine:
    free_migration_engine();
err_queue:
    free_migration_queue();
err:
    return -1;
}

void stop_monitoring(void)
{
    if ( !monitoring_started )
        return;
    stats_end();

    if ( ibs_capable() )
        disable_monitoring_ibs();
    else if ( pebs_capable() )
        disable_monitoring_pebs();

    monitoring_started = 0;

    free_migration_engine();
    free_migration_queue();

    display_stats();
}

 /*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
