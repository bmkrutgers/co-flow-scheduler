// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/sched/sch_fq.c Fair Queue Packet Scheduler (per flow pacing)
 *
 *  Copyright (C) 2013-2015 Eric Dumazet <edumazet@google.com>
 *
 *  Meant to be mostly used for locally generated traffic :
 *  Fast classification depends on skb->sk being set before reaching us.
 *  If not, (router workload), we use rxhash as fallback, with 32 bits wide
 * hash. All packets belonging to a socket are considered as a 'flow'.
 *
 *  Flows are dynamically allocated and stored in a hash table of RB trees
 *  They are also part of one Round Robin 'queues' (new or old flows)
 *
 *  Burst avoidance (aka pacing) capability :
 *
 *  Transport (eg TCP) can set in sk->sk_pacing_rate a rate, enqueue a
 *  bunch of packets, and this packet scheduler adds delay between
 *  packets to respect rate limitation.
 *
 *  enqueue() :
 *   - lookup one RB tree (out of 1024 or more) to find the flow.
 *     If non existent flow, create it, add it to the tree.
 *     Add skb to the per flow list of skb (fifo).
 *   - Use a special fifo for high prio packets
 *
 *  dequeue() : serves flows in Round Robin
 *  Note : When a flow becomes empty, we do not immediately remove it from
 *  rb trees, for performance reasons (its expected to send additional packets,
 *  or SLAB cache will reuse socket for another flow)
 */

#include <linux/errno.h>
#include <linux/hash.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/prefetch.h>
#include <linux/rbtree.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/tcp_states.h>
#include "fqtest.h"

/*
 * f->tail and f->age share the same location.
 * We can use the low order bit to differentiate if this location points
 * to a sk_buff or contains a jiffies value, if we force this value to be odd.
 * This assumes f->tail low order bit must be 0 since alignof(struct sk_buff) >=
 * 2
 */
static void fq_flow_set_detached(struct fq_flow *f)
{
    f->age = jiffies | 1UL;
}

static bool fq_flow_is_detached(const struct fq_flow *f)
{
    return !!(f->age & 1UL);
}

/* special value to mark a throttled flow (not on old/new list) */
static struct fq_flow throttled;

static bool fq_flow_is_throttled(const struct fq_flow *f)
{
    return f->next == &throttled;
}

static void fq_flow_unset_throttled(struct fq_sched_data *q,
                                    struct fq_flow *f)
{
    rb_erase(&f->rate_node, &q->delayed);
    q->throttled_flows--;
    fq_flow_add_tail(&q->old_flows, f);
}

static void fq_flow_set_throttled(struct fq_sched_data *q, struct fq_flow *f)
{
    struct rb_node **p = &q->delayed.rb_node, *parent = NULL;

    while (*p)
        {
            struct fq_flow *aux;

            parent = *p;
            aux = rb_entry(parent, struct fq_flow, rate_node);
            if (f->time_next_packet >= aux->time_next_packet)
                p = &parent->rb_right;
            else
                p = &parent->rb_left;
        }
    rb_link_node(&f->rate_node, parent, p);
    rb_insert_color(&f->rate_node, &q->delayed);
    q->throttled_flows++;
    q->stat_throttled++;

    f->next = &throttled;
    if (q->time_next_delayed_flow > f->time_next_packet)
        q->time_next_delayed_flow = f->time_next_packet;
}

static struct kmem_cache *fq_flow_cachep __read_mostly;

/* limit number of collected flows per round */
#define FQ_GC_MAX 8
#define FQ_GC_AGE (3 * HZ)

static bool fq_gc_candidate(const struct fq_flow *f)
{
    return fq_flow_is_detached(f) && time_after(jiffies, f->age + FQ_GC_AGE);
}

static void fq_gc(struct fq_sched_data *q, struct rb_root *root,
                  struct sock *sk)
{
    struct rb_node **p, *parent;
    void *tofree[FQ_GC_MAX];
    struct fq_flow *f;
    int i, fcnt = 0;

    p = &root->rb_node;
    parent = NULL;
    while (*p)
        {
            parent = *p;

            f = rb_entry(parent, struct fq_flow, fq_node);
            if (f->sk == sk) break;

            if (fq_gc_candidate(f))
                {
                    tofree[fcnt++] = f;
                    if (fcnt == FQ_GC_MAX) break;
                }

            if (f->sk > sk)
                p = &parent->rb_right;
            else
                p = &parent->rb_left;
        }

    if (!fcnt) return;

    for (i = fcnt; i > 0;)
        {
            f = tofree[--i];
            rb_erase(&f->fq_node, root);
        }
    q->flows -= fcnt;
    q->inactive_flows -= fcnt;
    q->stat_gc_flows += fcnt;

    kmem_cache_free_bulk(fq_flow_cachep, fcnt, tofree);
}


int ipow(int base, int exp)
{
    int result = 1;
    for (;;)
        {
            if (exp & 1)
                result *= base;
            exp >>= 1;
            if (!exp)
                break;
            base *= base;
        }

    return result;
}


static struct fq_flow *fq_classify(struct sk_buff *skb,
                                   struct fq_sched_data *q)
{
    struct rb_node **p, *parent;
    struct sock *sk = skb->sk;
    struct rb_root *root;
    struct fq_flow *f;

    // printk("In add values address pair is  : %lld \n ", sk->sk_portpair);
    // printk("In add values destination port is  : %lld \n ", sk->sk_dport);
    // printk("In add values hash pair is  : %d \n ", skb_get_hash(skb));

    /* warning: no starvation prevention... */
    if (unlikely((skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL))
        return &q->internal;

    /* SYNACK messages are attached to a TCP_NEW_SYN_RECV request socket
     * or a listener (SYNCOOKIE mode)
     * 1) request sockets are not full blown,
     *    they do not contain sk_pacing_rate
     * 2) They are not part of a 'flow' yet
     * 3) We do not want to rate limit them (eg SYNFLOOD attack),
     *    especially if the listener set SO_MAX_PACING_RATE
     * 4) We pretend they are orphaned
     */
    if (!sk || sk_listener(sk))
        {
            unsigned long hash = skb_get_hash(skb) & q->orphan_mask;

            /* By forcing low order bit to 1, we make sure to not
             * collide with a local flow (socket pointers are word aligned)
             */
            sk = (struct sock *)((hash << 1) | 1UL);

            // printk("hash value in fq classify  after sk creation : %lu \n ", hash );

            skb_orphan(skb);
        }
    else if (sk->sk_state == TCP_CLOSE)
        {
            unsigned long hash = skb_get_hash(skb) & q->orphan_mask;

            // printk("hash  value in fq classify in else if  of each flow is  : %lu \n
            // ",hash );
            /*
             * Sockets in TCP_CLOSE are non connected.
             * Typical use case is UDP sockets, they can send packets
             * with sendto() to many different destinations.
             * We probably could use a generic bit advertising
             * non connected sockets, instead of sk_state == TCP_CLOSE,
             * if we care enough.
             */
            sk = (struct sock *)((hash << 1) | 1UL);
        }

    root = &q->fq_root[hash_ptr(sk, q->fq_trees_log)];

    if (q->flows >= (2U << q->fq_trees_log) && q->inactive_flows > q->flows / 2)
        fq_gc(q, root, sk);

    p = &root->rb_node;
    parent = NULL;
    while (*p)
        {
            parent = *p;

            f = rb_entry(parent, struct fq_flow, fq_node);
            if (f->sk == sk)
                {
                    /* socket might have been reallocated, so check
                     * if its sk_hash is the same.
                     * It not, we need to refill credit with
                     * initial quantum
                     */
                    int lengthOfarray = 0;

                    int i;

                    for (i = 0; i < (sizeof(pFlowid) / sizeof(pFlowid[0])); i++)
                        {
                            lengthOfarray++;
                        }
                    if (unlikely(skb->sk == sk && f->socket_hash != sk->sk_hash))
                        {
                            f->credit = q->initial_quantum;
                            f->socket_hash = sk->sk_hash;

                            if (q->rate_enable)
                                smp_store_release(&sk->sk_pacing_status, SK_PACING_FQ);
                            if (fq_flow_is_throttled(f)) fq_flow_unset_throttled(q, f);
                            f->time_next_packet = 0ULL;
                        }
                    return f;
                }
            if (f->sk > sk)
                p = &parent->rb_right;
            else
                p = &parent->rb_left;
        }

    f = kmem_cache_zalloc(fq_flow_cachep, GFP_ATOMIC | __GFP_NOWARN);
    if (unlikely(!f))
        {
            q->stat_allocation_errors++;
            return &q->internal;
        }
    /* f->t_root is already zeroed after kmem_cache_zalloc() */

    fq_flow_set_detached(f);
    f->sk = sk;
    if (skb->sk == sk)
        {
            f->socket_hash = sk->sk_hash;
            if (q->rate_enable) smp_store_release(&sk->sk_pacing_status, SK_PACING_FQ);
        }
    f->credit = q->initial_quantum;

    rb_link_node(&f->fq_node, parent, p);
    rb_insert_color(&f->fq_node, root);

    q->flows++;
    q->inactive_flows++;
    // printk("flow hash in after classification  : %u \n ",f->socket_hash );
    return f;
}

static struct sk_buff *fq_peek(struct fq_flow *flow)
{
    struct sk_buff *skb = skb_rb_first(&flow->t_root);
    struct sk_buff *head = flow->head;

    if (!skb) return head;

    if (!head) return skb;

    if (fq_skb_cb(skb)->time_to_send < fq_skb_cb(head)->time_to_send) return skb;
    return head;
}

static void fq_erase_head(struct Qdisc *sch, struct fq_flow *flow,
                          struct sk_buff *skb)
{
    if (skb == flow->head)
        {
            flow->head = skb->next;
        }
    else
        {
            rb_erase(&skb->rbnode, &flow->t_root);
            skb->dev = qdisc_dev(sch);
        }
}

/* Remove one skb from flow queue.
 * This skb must be the return value of prior fq_peek().
 */
static void fq_dequeue_skb(struct Qdisc *sch, struct fq_flow *flow,
                           struct sk_buff *skb)
{
    fq_erase_head(sch, flow, skb);
    skb_mark_not_on_list(skb);
    flow->qlen--;
    qdisc_qstats_backlog_dec(sch, skb);
    sch->q.qlen--;
}

static void flow_queue_add(struct fq_flow *flow, struct sk_buff *skb)
{
    struct rb_node **p, *parent;
    struct sk_buff *head, *aux;

    head = flow->head;
    if (!head ||
            fq_skb_cb(skb)->time_to_send >= fq_skb_cb(flow->tail)->time_to_send)
        {
            if (!head)
                flow->head = skb;
            else
                flow->tail->next = skb;
            flow->tail = skb;
            skb->next = NULL;
            return;
        }

    p = &flow->t_root.rb_node;
    parent = NULL;

    while (*p)
        {
            parent = *p;
            aux = rb_to_skb(parent);
            if (fq_skb_cb(skb)->time_to_send >= fq_skb_cb(aux)->time_to_send)
                p = &parent->rb_right;
            else
                p = &parent->rb_left;
        }
    // printk("In add values skb after classification to check in add is  : %d \n
    // ", skb_get_hash(skb));
    rb_link_node(&skb->rbnode, parent, p);
    rb_insert_color(&skb->rbnode, &flow->t_root);
}

static bool fq_packet_beyond_horizon(const struct sk_buff *skb,
                                     const struct fq_sched_data *q)
{
    return unlikely((s64)skb->tstamp > (s64)(q->ktime_cache + q->horizon));
}

static int fq_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                      struct sk_buff **to_free)
{
    struct fq_sched_data *q = qdisc_priv(sch);

    printk("In enqueue adding flow data \n ");


    printk("f1->sourceport  : %u \n ", q->f1_sourceport);

    printk("f2->sourceport  : %u \n ", q->f2_sourceport);

    printk("f1->destport  :   %u \n ", q->f1_destport);

    printk("f2->destport  :   %u \n ", q->f2_destport);


    struct fq_flow *f;

    if (unlikely(sch->q.qlen >= sch->limit)) return qdisc_drop(skb, sch, to_free);

    if (!skb->tstamp)
        {
            fq_skb_cb(skb)->time_to_send = q->ktime_cache = ktime_get_ns();

        }
    else
        {
            /* Check if packet timestamp is too far in the future.
             * Try first if our cached value, to avoid ktime_get_ns()
             * cost in most cases.
             */
            if (fq_packet_beyond_horizon(skb, q))
                {
                    /* Refresh our cache and check another time */
                    q->ktime_cache = ktime_get_ns();
                    if (fq_packet_beyond_horizon(skb, q))
                        {
                            if (q->horizon_drop)
                                {
                                    q->stat_horizon_drops++;
                                    return qdisc_drop(skb, sch, to_free);
                                }
                            q->stat_horizon_caps++;
                            skb->tstamp = q->ktime_cache + q->horizon;
                        }
                }
            fq_skb_cb(skb)->time_to_send = skb->tstamp;
        }

    f = fq_classify(skb, q);



    if (unlikely(f->qlen >= q->flow_plimit && f != &q->internal))
        {
            q->stat_flows_plimit++;
            return qdisc_drop(skb, sch, to_free);
        }

    f->qlen++;
    qdisc_qstats_backlog_inc(sch, skb);
    if (fq_flow_is_detached(f))
        {


            fq_flow_add_tail(&q->new_flows, f);

            printk("In the Enqueue function and flow being added to the new flow data \n");



            int i;

            //pFlowid[0] = -1;
            //pFlowid[1] = -1;


            //if(skb->sk->sk_num != NULL)

            //sport = skb->sk->sk_num;

            //sport = 7869;

            //dport = skb->sk->sk_dport;

            //const __portpair ports = INET_COMBINED_PORTS(sport, dport);

            // printk( "Source Port Number of flow is  : %d \n ", sport);

            //printk("destination port of flow is  : %d \n ", dport);

            //printk("skb get hash value  : %d \n ", skb_get_hash(skb));


            printk("checking age of the flow \n");


            if (time_after(jiffies, f->age + q->flow_refill_delay))
                f->credit = max_t(u32, f->credit, q->quantum);
            q->inactive_flows--;
        }

    /* Note: this overwrites f->age */

    printk("hash of flow value : %u \n ", (f->socket_hash & q->orphan_mask) );

    printk("skb get hash value  : %u \n ", skb_get_hash(skb));

    //unsigned long pHash = skb_get_hash(skb) & q->orphan_mask;



    //sport = skb->sk->sk_num;

    //struct sock *sk = skb->sk;

    //sport = skb->sk_num;

    struct tcphdr *hdr = tcp_hdr(skb);

    struct iphdr *iph = ip_hdr(skb);


    sport = ntohs(hdr->source);

    dport = ntohs(hdr->dest);


    printk( "Source Port Number of flow is  : %d \n ", sport);

    printk( "Deestination Port Number of flow is  : %d \n ", dport);

    unsigned long pHash = sport;

    //skb->get_port(skb, sport);


    printk("pHash value  : %ld \n ", pHash);

    f->socket_hash = pHash;

    printk("flow hash value  : %d \n ", f->socket_hash);



    if (sport == q -> f1_sourceport)
        {
            if (pHash != pFlowid[1])
                pFlowid[0] = pHash;
            printk("flowid[0] value  : %d \n ", pFlowid[0]);

        }

    if (sport == q -> f2_sourceport)
        {

            if (pHash != pFlowid[0])
                pFlowid[1] = pHash;
            printk("flowid[1] value  : %d \n ", pFlowid[1]);

        }
        
        
        if(sport == q->f1_sourceport)
        printk("enqueue : A \n ");

    else if(sport == q->f2_sourceport)
        printk("enqueue : B \n ");

    else if(sport == 46732)
        printk("enqueue : C \n ");





    printk("in enqueue adding skb to the flow \n");

    if ( (pHash == pFlowid[0]) )
        {
            flow_queue_add(f, skb);

            if ((pHash == pFlowid[0]) && (ucounter_flow_1 == 0))
                {

                    time_first_flow_1 = ktime_get_ns();

                }
            ucounter_flow_1++;
            printk("flow one  increment counter value is %d \n", ucounter_flow_1);

            /*  struct fq_flow_head *head;

            struct fq_flow *g;

              head = &q->new_flows;

              g = head->first;

              Promotecoflows( & q -> old_flows, & q -> new_flows, & q -> co_flows, g, coflow,
                              Flowidarray, lengthOfarray);
            */

        }


    else if ((pHash == pFlowid[1]))
        {
            flow_queue_add(f, skb);

            if(pHash == pFlowid[1] && (ucounter_flow_2 == 0))

                {

                    time_first_flow_2 = ktime_get_ns();


                }
            ucounter_flow_2++;
            printk("flow two  increment counter value is %d \n", ucounter_flow_2);

            /*  struct fq_flow_head *head;

            struct fq_flow *g;

             head = &q->new_flows;

             g = head->first;

             Promotecoflows( & q -> old_flows, & q -> new_flows, & q -> co_flows, g, coflow,
                             Flowidarray, lengthOfarray);
            */

        }

    else
        {
            printk("in enqueue where non co flows being added \n");
            flow_queue_add(f, skb);

        }

    time_first = min_t(u64, time_first_flow_2, time_first_flow_1);

    ucounter = ucounter_flow_1 + ucounter_flow_2;

    printk("over all flow increment counter value is %d \n", ucounter);


    if (unlikely(f == &q->internal))
        {
            q->stat_internal_packets++;
        }
    sch->q.qlen++;

    return NET_XMIT_SUCCESS;

}

static void fq_check_throttled(struct fq_sched_data *q, u64 now)
{
    unsigned long sample;
    struct rb_node *p;

    if (q->time_next_delayed_flow > now) return;

    /* Update unthrottle latency EWMA.
     * This is cheap and can help diagnosing timer/latency problems.
     */
    sample = (unsigned long)(now - q->time_next_delayed_flow);
    q->unthrottle_latency_ns -= q->unthrottle_latency_ns >> 3;
    q->unthrottle_latency_ns += sample >> 3;

    q->time_next_delayed_flow = ~0ULL;
    while ((p = rb_first(&q->delayed)) != NULL)
        {
            struct fq_flow *f = rb_entry(p, struct fq_flow, rate_node);

            if (f->time_next_packet > now)
                {
                    q->time_next_delayed_flow = f->time_next_packet;
                    break;
                }
            fq_flow_unset_throttled(q, f);
        }
}

static struct sk_buff *fq_dequeue(struct Qdisc *sch)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    struct fq_flow_head *head, *lscfhead, *lsnfhead, *lsofhead;
    struct sk_buff *skb;
    struct fq_flow *f;
    unsigned long rate;
    u32 plen;
    u64 now;
    int lengthOfarray =2;
    int i;
    int flag[lengthOfarray];

    for (i = 0; i < lengthOfarray; i++)
        {
            flag[i] = 0;
        }


    if (!sch->q.qlen) return NULL;

    skb = fq_peek(&q->internal);
    if (unlikely(skb))
        {
            fq_dequeue_skb(sch, &q->internal, skb);
            goto out;
        }

    q->ktime_cache = now = ktime_get_ns();
    fq_check_throttled(q, now);


    printk("In dequeue \n");



    //printk("ucounter value after packet of coflow has been added : %d \n ", ucounter);

    if (ucounter)
        {

            time_nw = ktime_get_ns();

            time_elapsed = time_nw - time_first;

            printk("time elapsed from the first packet of co-flow queue that has been added : %llu \n ", time_elapsed);

        }

    if ( (ucounter && (ucounter%2) == 0) && (ucounter_flow_1) && (ucounter_flow_2) && !(flipflag) )

        {

            printk("time elapsed from the first packet of co-flow queue that has been added to co-flow being dequeued :%llu \n ", time_elapsed);

            printk(" decrement counter value is  :%d \n ", ucounter);

            printk("setting flag for co-flow dequeuing and resetting all counters \n");

            flipflag = 1;

            dcounter = ucounter;

            dcounter_flow_1 = ucounter_flow_1;

            dcounter_flow_2 = ucounter_flow_2;

            ucounter = 0;

            ucounter_flow_1 = 0;

            ucounter_flow_2 = 0;

            dcounter_flag_1 = 1;

            dcounter_flag_2 = 1;


            head = &q->new_flows;

            f = head->first;

            Promotecoflows( & q -> old_flows, & q -> new_flows, & q -> co_flows, f, coflow,
                            Flowidarray, lengthOfarray);

        }




begin:
    printk("In co-flows \n");
    if(flipflag)
        {
            printk("flipflag has been set \n");
            head = &q->co_flows;
            if (!head->first)
                {

                    printk("co-flows are empty \n");

                }

            else

                {

                    printk("co-flows not  empty \n");

                }

        }
    else
        {
            printk("flipflag has not been set \n");
            head = &q->new_flows;
        }
    if (!head->first)
        {
            printk("In new-flows \n");
            head = &q->new_flows;
            if (!head->first)
                {
                    printk("In old-flows \n");
                    head = &q->old_flows;
                    if (!head->first)
                        {
                            printk("End has reached \n");


                            if (q->time_next_delayed_flow != ~0ULL)
                                qdisc_watchdog_schedule_range_ns(
                                    &q->watchdog, q->time_next_delayed_flow, q->timer_slack);
                            return NULL;
                        }
                }
        }


    f = head->first;

    int rValue = valuePresentInArray(f->socket_hash, Flowidarray, lengthOfarray);



    if ((rValue != -1) && (head != & q -> co_flows))
        {

            printk("promote clows \n ");

            Promotecoflows( & q -> old_flows, & q -> new_flows, & q -> co_flows, f, coflow,
                            pFlowid, lengthOfarray);
            goto begin;

        }


    if (!(dcounter) && (head == & q -> co_flows) )
        {

            printk("breach finished flip flag has to be reset\n");

            flipflag = 0;

            goto begin;


        }


    if (!(dcounter_flow_1) && (head == &q->co_flows) && dcounter && dcounter_flag_1)
        {

            printk("flow 1  finished its quota \n");

            dcounter_flag_1 = 0;

            head->first = f->next;

            fq_flow_add_tail(&q->new_flows, f);

            goto begin;
        }




    if (!(dcounter_flow_2) && (head == &q->co_flows) && dcounter && dcounter_flag_2)
        {

            printk("flow 2  finished its quota \n");

            head->first = f->next;

            dcounter_flag_2 = 0;


            fq_flow_add_tail(&q->new_flows, f);

            goto begin;

        }




    if ((rValue == 0) && (flipflag) && (head == & q -> co_flows) && dcounter_flow_1)
        {

            printk("decrement counter of flow 1 and overvall decrement of counter value before decrement : %d  and %d \n ", dcounter_flow_1, dcounter);

            dcounter_flow_1--;

            dcounter--;

            printk("decrement counter of flow 1 and overvall decrement of counter value after decrement : %d  and %d \n ", dcounter_flow_1, dcounter);

        }


    if ((rValue == 1) && (flipflag) && (head == & q -> co_flows) && dcounter_flow_2)
        {

            printk("decrement counter of flow 2 and overvall decrement of counter value before decrement : %d  and %d \n ", dcounter_flow_2, dcounter);

            dcounter_flow_2--;

            dcounter--;

            printk("decrement counter of flow 2 and overvall decrement of counter value after decrement : %d  and %d \n ", dcounter_flow_2, dcounter);

        }



    printk("credit of the flow before   : %d \n ", f->credit);
    if (f->credit <= 0)
        {
            f->credit += q->quantum;
            head->first = f->next;
            fq_flow_add_tail(&q->old_flows, f);
            printk("not enough credit \n");
            goto begin;
        }


    skb = fq_peek(f);
    if (skb)
        {
            u64 time_next_packet =
                max_t(u64, fq_skb_cb(skb)->time_to_send, f->time_next_packet);

            if (now < time_next_packet)
                {
                    head->first = f->next;
                    f->time_next_packet = time_next_packet;
                    fq_flow_set_throttled(q, f);

                    printk(" flow is throttled \n");

                    goto begin;

                }
            prefetch(&skb->end);
            if ((s64)(now - time_next_packet - q->ce_threshold) > 0)
                {
                    INET_ECN_set_ce(skb);
                    q->stat_ce_mark++;
                }
            fq_dequeue_skb(sch, f, skb);
            printk("sending the packet after dequeue \n");
        }
    else
        {
            printk("skb is empty\n");

            head->first = f->next;
            /* force a pass through old_flows to prevent starvation */
            if (( (head == &q->new_flows) ||(head == &q->co_flows) ) && q->old_flows.first)
                {
                    fq_flow_add_tail(&q->old_flows, f);
                }

            else
                {
                    fq_flow_set_detached(f);
                    q->inactive_flows++;
                }
            goto begin;

        }
    plen = qdisc_pkt_len(skb);
    printk("credit of the flow before   : %d \n ", f->credit);
    f->credit -= plen;
    printk("credit of the flow after   : %d \n ", f->credit);

    if (!q->rate_enable) goto out;

    rate = q->flow_max_rate;

    /* If EDT time was provided for this skb, we need to
     * update f->time_next_packet only if this qdisc enforces
     * a flow max rate.
     */
    if (!skb->tstamp)
        {
            if (skb->sk) rate = min(skb->sk->sk_pacing_rate, rate);

            if (rate <= q->low_rate_threshold)
                {
                    f->credit = 0;
                }
            else
                {
                    plen = max(plen, q->quantum);
                    if (f->credit > 0) goto out;
                }
        }
    if (rate != ~0UL)
        {
            u64 len = (u64)plen * NSEC_PER_SEC;

            if (likely(rate)) len = div64_ul(len, rate);
            /* Since socket rate can change later,
             * clamp the delay to 1 second.
             * Really, providers of too big packets should be fixed !
             */
            if (unlikely(len > NSEC_PER_SEC))
                {
                    len = NSEC_PER_SEC;
                    q->stat_pkts_too_long++;
                }
            /* Account for schedule/timers drifts.
             * f->time_next_packet was set when prior packet was sent,
             * and current time (@now) can be too late by tens of us.
             */
            if (f->time_next_packet) len -= min(len / 2, now - f->time_next_packet);
            f->time_next_packet = now + len;
        }
out:
    qdisc_bstats_update(sch, skb);
    
    printk("flow hash value in out  : %u \n ", f->socket_hash);

    printk("in out sending the packet \n");

    


    /*struct tcphdr *hdr = tcp_hdr(skb);

    struct iphdr *iph = ip_hdr(skb);

    int sport = ntohs(hdr->source);
    
    //printk("flow hash value in out  : %d \n ", f->socket_hash);


    if(f->socket_hash == q->f1_sourceport)
        printk("dequeue : A \n ");

    else if(f->socket_hash == q->f2_sourceport)
        printk("dequeue : B \n ");

    else if (f->socket_hash == 46732)
        printk("dequeue : C \n ");*/

    return skb;
}

static void fq_flow_purge(struct fq_flow *flow)
{
    struct rb_node *p = rb_first(&flow->t_root);

    while (p)
        {
            struct sk_buff *skb = rb_to_skb(p);

            p = rb_next(p);
            rb_erase(&skb->rbnode, &flow->t_root);
            rtnl_kfree_skbs(skb, skb);
        }
    rtnl_kfree_skbs(flow->head, flow->tail);
    flow->head = NULL;
    flow->qlen = 0;
}

static void fq_reset(struct Qdisc *sch)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    struct rb_root *root;
    struct rb_node *p;
    struct fq_flow *f;
    unsigned int idx;

    sch->q.qlen = 0;
    sch->qstats.backlog = 0;

    fq_flow_purge(&q->internal);

    if (!q->fq_root) return;

    for (idx = 0; idx < (1U << q->fq_trees_log); idx++)
        {
            root = &q->fq_root[idx];
            while ((p = rb_first(root)) != NULL)
                {
                    f = rb_entry(p, struct fq_flow, fq_node);
                    rb_erase(p, root);

                    fq_flow_purge(f);

                    kmem_cache_free(fq_flow_cachep, f);
                }
        }
    q->new_flows.first = NULL;
    q->old_flows.first = NULL;
    q->co_flows.first = NULL;
    q->delayed = RB_ROOT;
    q->flows = 0;
    q->inactive_flows = 0;
    q->throttled_flows = 0;
}

static void fq_rehash(struct fq_sched_data *q, struct rb_root *old_array,
                      u32 old_log, struct rb_root *new_array, u32 new_log)
{
    struct rb_node *op, **np, *parent;
    struct rb_root *oroot, *nroot;
    struct fq_flow *of, *nf;
    int fcnt = 0;
    u32 idx;

    for (idx = 0; idx < (1U << old_log); idx++)
        {
            oroot = &old_array[idx];
            while ((op = rb_first(oroot)) != NULL)
                {
                    rb_erase(op, oroot);
                    of = rb_entry(op, struct fq_flow, fq_node);
                    if (fq_gc_candidate(of))
                        {
                            fcnt++;
                            kmem_cache_free(fq_flow_cachep, of);
                            continue;
                        }
                    nroot = &new_array[hash_ptr(of->sk, new_log)];

                    np = &nroot->rb_node;
                    parent = NULL;
                    while (*np)
                        {
                            parent = *np;

                            nf = rb_entry(parent, struct fq_flow, fq_node);
                            BUG_ON(nf->sk == of->sk);

                            if (nf->sk > of->sk)
                                np = &parent->rb_right;
                            else
                                np = &parent->rb_left;
                        }

                    rb_link_node(&of->fq_node, parent, np);
                    rb_insert_color(&of->fq_node, nroot);
                }
        }
    q->flows -= fcnt;
    q->inactive_flows -= fcnt;
    q->stat_gc_flows += fcnt;
}

static void fq_free(void *addr)
{
    kvfree(addr);
}

static int fq_resize(struct Qdisc *sch, u32 log)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    struct rb_root *array;
    void *old_fq_root;
    u32 idx;

    if (q->fq_root && log == q->fq_trees_log) return 0;

    /* If XPS was setup, we can allocate memory on right NUMA node */
    array = kvmalloc_node(sizeof(struct rb_root) << log,
                          GFP_KERNEL | __GFP_RETRY_MAYFAIL,
                          netdev_queue_numa_node_read(sch->dev_queue));
    if (!array) return -ENOMEM;

    for (idx = 0; idx < (1U << log); idx++) array[idx] = RB_ROOT;

    sch_tree_lock(sch);

    old_fq_root = q->fq_root;
    if (old_fq_root) fq_rehash(q, old_fq_root, q->fq_trees_log, array, log);

    q->fq_root = array;
    q->fq_trees_log = log;

    sch_tree_unlock(sch);

    fq_free(old_fq_root);

    return 0;
}

static const struct nla_policy fq_policy[TCA_FQ_MAX + 1] =
{
    [TCA_FQ_UNSPEC] = {.strict_start_type = TCA_FQ_TIMER_SLACK},

    [TCA_FQ_PLIMIT] = {.type = NLA_U32},
    [TCA_FQ_FLOW_PLIMIT] = {.type = NLA_U32},
    [TCA_FQ_QUANTUM] = {.type = NLA_U32},
    [TCA_FQ_INITIAL_QUANTUM] = {.type = NLA_U32},
    [TCA_FQ_RATE_ENABLE] = {.type = NLA_U32},
    [TCA_FQ_FLOW_DEFAULT_RATE] = {.type = NLA_U32},
    [TCA_FQ_FLOW_MAX_RATE] = {.type = NLA_U32},
    [TCA_FQ_BUCKETS_LOG] = {.type = NLA_U32},
    [TCA_FQ_FLOW_REFILL_DELAY] = {.type = NLA_U32},
    [TCA_FQ_ORPHAN_MASK] = {.type = NLA_U32},
    [TCA_FQ_LOW_RATE_THRESHOLD] = {.type = NLA_U32},
    [TCA_FQ_CE_THRESHOLD] = {.type = NLA_U32},
    [TCA_FQ_TIMER_SLACK] = {.type = NLA_U32},
    [TCA_FQ_HORIZON] = {.type = NLA_U32},
    [TCA_FQ_HORIZON_DROP] = {.type = NLA_U8},
    [TCA_FQ_F1_SOURCEPORT] = {.type = NLA_U32},
    [TCA_FQ_F2_SOURCEPORT] = {.type = NLA_U32},
    [TCA_FQ_F1_DESTPORT] = {.type = NLA_U32},
    [TCA_FQ_F2_DESTPORT] = {.type = NLA_U32},
};

static int fq_change(struct Qdisc *sch, struct nlattr *opt,
                     struct netlink_ext_ack *extack)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    struct nlattr *tb[TCA_FQ_MAX + 1];
    int err, drop_count = 0;
    unsigned drop_len = 0;
    u32 fq_log;

    if (!opt) return -EINVAL;

    err = nla_parse_nested_deprecated(tb, TCA_FQ_MAX, opt, fq_policy, NULL);
    if (err < 0) return err;

    sch_tree_lock(sch);

    fq_log = q->fq_trees_log;

    if (tb[TCA_FQ_BUCKETS_LOG])
        {
            u32 nval = nla_get_u32(tb[TCA_FQ_BUCKETS_LOG]);

            if (nval >= 1 && nval <= ilog2(256 * 1024))
                fq_log = nval;
            else
                err = -EINVAL;
        }
    if (tb[TCA_FQ_PLIMIT]) sch->limit = nla_get_u32(tb[TCA_FQ_PLIMIT]);

    if (tb[TCA_FQ_FLOW_PLIMIT])
        q->flow_plimit = nla_get_u32(tb[TCA_FQ_FLOW_PLIMIT]);

    if (tb[TCA_FQ_QUANTUM])
        {
            u32 quantum = nla_get_u32(tb[TCA_FQ_QUANTUM]);

            if (quantum > 0 && quantum <= (1 << 20))
                {
                    q->quantum = quantum;
                }
            else
                {
                    NL_SET_ERR_MSG_MOD(extack, "invalid quantum");
                    err = -EINVAL;
                }
        }

    if (tb[TCA_FQ_INITIAL_QUANTUM])
        q->initial_quantum = nla_get_u32(tb[TCA_FQ_INITIAL_QUANTUM]);

    if (tb[TCA_FQ_FLOW_DEFAULT_RATE])
        pr_warn_ratelimited("sch_fq: defrate %u ignored.\n",
                            nla_get_u32(tb[TCA_FQ_FLOW_DEFAULT_RATE]));

    if (tb[TCA_FQ_FLOW_MAX_RATE])
        {
            u32 rate = nla_get_u32(tb[TCA_FQ_FLOW_MAX_RATE]);

            q->flow_max_rate = (rate == ~0U) ? ~0UL : rate;
        }
    if (tb[TCA_FQ_LOW_RATE_THRESHOLD])
        q->low_rate_threshold = nla_get_u32(tb[TCA_FQ_LOW_RATE_THRESHOLD]);

    if (tb[TCA_FQ_RATE_ENABLE])
        {
            u32 enable = nla_get_u32(tb[TCA_FQ_RATE_ENABLE]);

            if (enable <= 1)
                q->rate_enable = enable;
            else
                err = -EINVAL;
        }

    if (tb[TCA_FQ_FLOW_REFILL_DELAY])
        {
            u32 usecs_delay = nla_get_u32(tb[TCA_FQ_FLOW_REFILL_DELAY]);

            q->flow_refill_delay = usecs_to_jiffies(usecs_delay);
        }

    if (tb[TCA_FQ_ORPHAN_MASK])
        q->orphan_mask = nla_get_u32(tb[TCA_FQ_ORPHAN_MASK]);

    if (tb[TCA_FQ_CE_THRESHOLD])
        q->ce_threshold = (u64)NSEC_PER_USEC * nla_get_u32(tb[TCA_FQ_CE_THRESHOLD]);

    if (tb[TCA_FQ_TIMER_SLACK])
        q->timer_slack = nla_get_u32(tb[TCA_FQ_TIMER_SLACK]);

    if (tb[TCA_FQ_HORIZON])
        q->horizon = (u64)NSEC_PER_USEC * nla_get_u32(tb[TCA_FQ_HORIZON]);

    if (tb[TCA_FQ_HORIZON_DROP])
        q->horizon_drop = nla_get_u8(tb[TCA_FQ_HORIZON_DROP]);

    if (tb[TCA_FQ_F1_SOURCEPORT])
        q->f1_sourceport = nla_get_u32(tb[TCA_FQ_F1_SOURCEPORT]);

    if (tb[TCA_FQ_F2_SOURCEPORT])
        q->f2_sourceport = nla_get_u32(tb[TCA_FQ_F2_SOURCEPORT]);

    if (tb[TCA_FQ_F1_DESTPORT])
        q->f1_destport = nla_get_u32(tb[TCA_FQ_F1_DESTPORT]);

    if (tb[TCA_FQ_F2_DESTPORT])
        q->f2_destport = nla_get_u32(tb[TCA_FQ_F2_DESTPORT]);

    if (!err)
        {
            sch_tree_unlock(sch);
            err = fq_resize(sch, fq_log);
            sch_tree_lock(sch);
        }
    while (sch->q.qlen > sch->limit)
        {
            struct sk_buff *skb = fq_dequeue(sch);
            printk("dequeue has taken place \n");

            if (!skb) break;
            drop_len += qdisc_pkt_len(skb);
            rtnl_kfree_skbs(skb, skb);
            drop_count++;
        }
    qdisc_tree_reduce_backlog(sch, drop_count, drop_len);

    sch_tree_unlock(sch);
    return err;
}

static void fq_destroy(struct Qdisc *sch)
{
    struct fq_sched_data *q = qdisc_priv(sch);

    fq_reset(sch);
    fq_free(q->fq_root);
    qdisc_watchdog_cancel(&q->watchdog);
}

static int fq_init(struct Qdisc *sch, struct nlattr *opt,
                   struct netlink_ext_ack *extack)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    int err;

    printk("new connections\n \n \n \n \n \n \n \n \n \n");

    sch->limit = 10000;
    q->flow_plimit = 100;
    q->quantum = 2 * psched_mtu(qdisc_dev(sch));
    q->initial_quantum = 10 * psched_mtu(qdisc_dev(sch));
    q->flow_refill_delay = msecs_to_jiffies(40);
    q->flow_max_rate = ~0UL;
    q->time_next_delayed_flow = ~0ULL;
    q->rate_enable = 1;
    q->new_flows.first = NULL;
    q->old_flows.first = NULL;
    q->co_flows.first = NULL;
    q->delayed = RB_ROOT;
    q->fq_root = NULL;
    q->fq_trees_log = ilog2(1024);
    q->orphan_mask = 1024 - 1;
    q->low_rate_threshold = 550000 / 8;
    q->f1_sourceport = 0;
    q->f2_sourceport = 0;
    q->f1_destport = 0;
    q->f2_destport = 0;

    q->timer_slack = 10 * NSEC_PER_USEC; /* 10 usec of hrtimer slack */

    q->horizon = 10ULL * NSEC_PER_SEC; /* 10 seconds */
    q->horizon_drop = 1; /* by default, drop packets beyond horizon */

    /* Default ce_threshold of 4294 seconds */
    q->ce_threshold = (u64)NSEC_PER_USEC * ~0U;

    qdisc_watchdog_init_clockid(&q->watchdog, sch, CLOCK_MONOTONIC);


    //testfq(sch,q);

    if (opt)
        err = fq_change(sch, opt, extack);
    else
        err = fq_resize(sch, q->fq_trees_log);

    return err;
}

static int fq_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    u64 ce_threshold = q->ce_threshold;
    u64 horizon = q->horizon;
    struct nlattr *opts;

    opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
    if (opts == NULL) goto nla_put_failure;

    /* TCA_FQ_FLOW_DEFAULT_RATE is not used anymore */

    do_div(ce_threshold, NSEC_PER_USEC);
    do_div(horizon, NSEC_PER_USEC);

    if (nla_put_u32(skb, TCA_FQ_PLIMIT, sch->limit) ||
            nla_put_u32(skb, TCA_FQ_FLOW_PLIMIT, q->flow_plimit) ||
            nla_put_u32(skb, TCA_FQ_QUANTUM, q->quantum) ||
            nla_put_u32(skb, TCA_FQ_INITIAL_QUANTUM, q->initial_quantum) ||
            nla_put_u32(skb, TCA_FQ_RATE_ENABLE, q->rate_enable) ||
            nla_put_u32(skb, TCA_FQ_FLOW_MAX_RATE,
                        min_t(unsigned long, q->flow_max_rate, ~0U)) ||
            nla_put_u32(skb, TCA_FQ_FLOW_REFILL_DELAY,
                        jiffies_to_usecs(q->flow_refill_delay)) ||
            nla_put_u32(skb, TCA_FQ_ORPHAN_MASK, q->orphan_mask) ||
            nla_put_u32(skb, TCA_FQ_LOW_RATE_THRESHOLD, q->low_rate_threshold) ||
            nla_put_u32(skb, TCA_FQ_CE_THRESHOLD, (u32)ce_threshold) ||
            nla_put_u32(skb, TCA_FQ_BUCKETS_LOG, q->fq_trees_log) ||
            nla_put_u32(skb, TCA_FQ_TIMER_SLACK, q->timer_slack) ||
            nla_put_u32(skb, TCA_FQ_HORIZON, (u32)horizon) ||
            nla_put_u8(skb, TCA_FQ_HORIZON_DROP, q->horizon_drop)||
            nla_put_u32(skb, TCA_FQ_F1_SOURCEPORT, q->f1_sourceport)||
            nla_put_u32(skb, TCA_FQ_F2_SOURCEPORT, q->f2_sourceport)||
            nla_put_u32(skb, TCA_FQ_F1_DESTPORT, q->f1_destport)||
            nla_put_u32(skb, TCA_FQ_F2_DESTPORT, q->f2_destport))
        goto nla_put_failure;

    return nla_nest_end(skb, opts);

nla_put_failure:
    return -1;
}

static int fq_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
    struct fq_sched_data *q = qdisc_priv(sch);
    struct tc_fq_qd_stats st;

    sch_tree_lock(sch);

    st.gc_flows = q->stat_gc_flows;
    st.highprio_packets = q->stat_internal_packets;
    st.tcp_retrans = 0;
    st.throttled = q->stat_throttled;
    st.flows_plimit = q->stat_flows_plimit;
    st.pkts_too_long = q->stat_pkts_too_long;
    st.allocation_errors = q->stat_allocation_errors;
    st.time_next_delayed_flow =
        q->time_next_delayed_flow + q->timer_slack - ktime_get_ns();
    st.flows = q->flows;
    st.inactive_flows = q->inactive_flows;
    st.throttled_flows = q->throttled_flows;
    st.unthrottle_latency_ns =
        min_t(unsigned long, q->unthrottle_latency_ns, ~0U);
    st.ce_mark = q->stat_ce_mark;
    st.horizon_drops = q->stat_horizon_drops;
    st.horizon_caps = q->stat_horizon_caps;
    sch_tree_unlock(sch);

    return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc_ops fq_qdisc_ops __read_mostly =
{
    .id = "fq",
    .priv_size = sizeof(struct fq_sched_data),

    .enqueue = fq_enqueue,
    .dequeue = fq_dequeue,
    .peek = qdisc_peek_dequeued,
    .init = fq_init,
    .reset = fq_reset,
    .destroy = fq_destroy,
    .change = fq_change,
    .dump = fq_dump,
    .dump_stats = fq_dump_stats,
    .owner = THIS_MODULE,
};

static int __init fq_module_init(void)
{
    int ret;

    fq_flow_cachep =
        kmem_cache_create("fq_flow_cache", sizeof(struct fq_flow), 0, 0, NULL);
    if (!fq_flow_cachep) return -ENOMEM;

    ret = register_qdisc(&fq_qdisc_ops);
    if (ret) kmem_cache_destroy(fq_flow_cachep);
    return ret;
}

static void __exit fq_module_exit(void)
{
    unregister_qdisc(&fq_qdisc_ops);
    kmem_cache_destroy(fq_flow_cachep);
}

module_init(fq_module_init) module_exit(fq_module_exit)
MODULE_AUTHOR("Eric Dumazet");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fair Queue Packet Scheduler");

