/*
 *  Copyright (c) 2017 Intel Corporation. All rights reserved.
 *  This software is available to you under the BSD license below:
 *
 *      Redistribution and use in source and binary forms, with or
 *      without modification, are permitted provided that the following
 *      conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <common.h>

#define MAX_MSG_SIZE (1<<23)
#define START_LEN 1

#define INC 2
#define TRIALS 500
#define WINDOW_SIZE 64
#define WARMUP 50

#define TRIALS_LARGE  100
#define WINDOW_SIZE_LARGE 64
#define WARMUP_LARGE  10
#define LARGE_MESSAGE_SIZE  8192

#define TARGET_SZ_MIN 8
#define TARGET_SZ_MAX 4096

/*atomics common */
#define ATOMICS_N_DTs 3
/*note: ignoring cswap/swap for now in verification */
#define ATOMICS_N_OPs 4
/*PE 0 is printing its latency, thus have it not be the INCAST PE*/
#define INCAST_PE 1

typedef enum {
    UNI_DIR,
    BI_DIR,
} bw_type;

typedef enum {
    STYLE_PUT,
    STYLE_GET,
    STYLE_RMA,
    STYLE_ATOMIC
} bw_style;

typedef enum {
    FIRST_HALF,
    SECOND_HALF,
    FULL_SET
} red_PE_set;

typedef enum {
    COMM_PAIRWISE,
    COMM_INCAST
} comm_style;

typedef enum {
    B,
    KB,
    MB
} bw_units;

typedef struct perf_metrics {
    unsigned long int start_len, max_len;
    unsigned long int size_inc, trials;
    unsigned long int window_size, warmup;
    int validate;
    int target_data;
    int my_node, num_pes, sztarget, szinitiator, midpt;
    bw_units unit;
    char *src, *dest;
    const char *bw_type;
    bw_type type;
    comm_style cstyle;
    bw_style bwstyle;
} perf_metrics_t;

long red_psync[SHMEM_REDUCE_SYNC_SIZE];

/*default settings if no input is provided */
void static data_init(perf_metrics_t * data) {
    data->start_len = START_LEN;
    data->max_len = MAX_MSG_SIZE;
    data->size_inc = INC;
    data->trials = TRIALS;
    data->window_size = WINDOW_SIZE; /*back-to-back msg stream*/
    data->warmup = WARMUP; /*number of initial iterations to skip*/
    data->unit = MB;
    data->validate = false;
    data->target_data = false;
    data->my_node = shmem_my_pe();
    data->num_pes = shmem_n_pes();
    data->midpt = data->num_pes/2;
    data->sztarget = data->midpt;
    data->szinitiator = data->midpt;
    data->src = NULL;
    data->dest = NULL;
    data->cstyle = COMM_PAIRWISE;
    data->bwstyle = STYLE_RMA;
}

static const char * dt_names [] = { "int", "long", "longlong" };

void static bi_dir_data_init(perf_metrics_t * data) {
    data->bw_type = "Bi-directional Bandwidth";
    data->type = BI_DIR;
}

void static uni_dir_data_init(perf_metrics_t * data) {
    data->bw_type = "Uni-directional Bandwidth";
    data->type = UNI_DIR;
}


int static inline partner_node(perf_metrics_t my_info)
{
    if(my_info.num_pes == 1)
        return 0;

    if(my_info.cstyle == COMM_PAIRWISE) {
        int pairs = my_info.midpt;

        return (my_info.my_node < pairs ?
            ((my_info.my_node + pairs) % (my_info.sztarget + pairs)) :
            (my_info.my_node - pairs) % my_info.szinitiator);
    } else {
        assert(my_info.cstyle == COMM_INCAST);
        return INCAST_PE;
    }
}

int static inline streaming_node(perf_metrics_t my_info)
{
    if(my_info.cstyle == COMM_PAIRWISE) {
        return (my_info.my_node < my_info.szinitiator);
    } else {
        assert(my_info.cstyle == COMM_INCAST);
        return true;
    }
}

int static inline target_node(perf_metrics_t my_info)
{
    return (my_info.my_node >= my_info.midpt &&
        (my_info.my_node < (my_info.midpt + my_info.sztarget)));
}

/* put/get bw use opposite streaming/validate nodes */
red_PE_set static inline validation_set(perf_metrics_t my_info, int *nPEs)
{
    if(my_info.cstyle == COMM_PAIRWISE) {
        if(streaming_node(my_info)) {
            *nPEs = my_info.szinitiator;
            return FIRST_HALF;
        } else if(target_node(my_info)) {
            *nPEs = my_info.sztarget;
            return SECOND_HALF;
        } else {
            fprintf(stderr, "Warning: you are getting data from a node that " \
                "wasn't a part of the perf set \n ");
        }
    } else {
        assert(my_info.cstyle == COMM_INCAST);
        *nPEs = my_info.num_pes;
        return FULL_SET;
    }
}

/**************************************************************/
/*                   Input Checking                           */
/**************************************************************/

void static command_line_arg_check(int argc, char *argv[],
                                   perf_metrics_t *metric_info) {
    int ch, error = false;
    extern char *optarg;

    /* check command line args */
    while ((ch = getopt(argc, argv, "e:s:n:w:p:r:l:kbvt")) != EOF) {
        switch (ch) {
        case 's':
            metric_info->start_len = strtoul(optarg, (char **)NULL, 0);
            if ( metric_info->start_len < 1 ) metric_info->start_len = 1;
            if(!is_pow_of_2(metric_info->start_len)) error = true;
            break;
        case 'e':
            metric_info->max_len = strtoul(optarg, (char **)NULL, 0);
            if(!is_pow_of_2(metric_info->max_len)) error = true;
            if(metric_info->max_len < metric_info->start_len) error = true;
            break;
        case 'n':
            metric_info->trials = strtoul(optarg, (char **)NULL, 0);
            if(metric_info->trials < (metric_info->warmup*2)) error = true;
            break;
        case 'p':
            metric_info->warmup = strtoul(optarg, (char **)NULL, 0);
            if(metric_info->warmup > (metric_info->trials/2)) error = true;
            break;
        case 'k':
            metric_info->unit = KB;
            break;
        case 'b':
            metric_info->unit = B;
            break;
        case 'v':
            metric_info->validate = true;
            if(metric_info->target_data) error = true;
            break;
        case 'w':
            metric_info->window_size = strtoul(optarg, (char **)NULL, 0);
            if(metric_info->target_data) error = true;
            break;
        case 't':
            metric_info->target_data = true;
            metric_info->window_size = 1;
            if(metric_info->validate) error = true;
            break;
        case 'r':
            metric_info->sztarget = strtoul(optarg, (char **)NULL, 0);
            if(metric_info->sztarget > metric_info->midpt ||
                metric_info->szinitiator < metric_info->midpt) error = true;
            break;
        case 'l':
            metric_info->szinitiator = strtoul(optarg, (char **)NULL, 0);
            if(metric_info->szinitiator > metric_info->midpt ||
                metric_info->sztarget < metric_info->midpt) error = true;
            break;
        default:
            error = true;
            break;
        }
    }

    /* filling in 8/4KB chunks into array alloc'd to max_len */
    if(metric_info->target_data) {
        metric_info->start_len = TARGET_SZ_MIN;
        if((metric_info->max_len <
            ((metric_info->trials + metric_info->warmup) * TARGET_SZ_MIN)) ||
            (metric_info->max_len <
            ((metric_info->trials + metric_info->warmup) * TARGET_SZ_MAX))) {
                error = true;
            }
    }

    if (error) {
        if (metric_info->my_node == 0) {
            fprintf(stderr, "Usage: \n[-s start_length] [-e end_length] "\
                    ": lengths should be a power of two \n" \
                    "[-n trials (must be greater than 2*warmup (default: x => 100))] \n"\
                    "[-p warm-up (see trials for value restriction)] \n"\
                    "[-w window size - iterations between completion, cannot use with -t] \n"\
                    "[-k (kilobytes/second)] [-b (bytes/second)] \n"\
                    "[-v (validate data stream)] \n"\
                    "[-t output data for target side (default is initiator,"\
                    " only use with put_bw),\n cannot be used in conjunction "\
                    "with validate, special sizes used, \ntrials" \
                    " + warmup * sizes (8/4KB) <= max length \n" \
                    "[-r number of nodes at target] \n" \
                    "[-l number of nodes at initiator] \n");
        }
#ifndef VERSION_1_0
        shmem_finalize();
#endif
        exit (-1);
    }
}

void static inline only_even_PEs_check(int my_node, int num_pes) {
    if (num_pes % 2 != 0) {
        if (my_node == 0) {
            fprintf(stderr, "can only use an even number of nodes\n");
        }
#ifndef VERSION_1_0
        shmem_finalize();
#endif
        exit(77);
    }
}

/**************************************************************/
/*                   Result Printing and Calc                 */
/**************************************************************/

void static print_atomic_results_header(perf_metrics_t metric_info) {
    printf("\nResults for %d PEs %lu trials with window size %lu ",
            metric_info.num_pes, metric_info.trials, metric_info.window_size);

    if (metric_info.cstyle == COMM_INCAST) {
        printf("using incast communication style\n");
    } else {
        assert(metric_info.cstyle == COMM_PAIRWISE);
        printf("using pairwise communication style\n");
    }

    printf("\nOperation           %s           "\
            "Message Rate%17sLatency\n", metric_info.bw_type, " ");

    if (metric_info.unit == MB) {
        printf("%19s in megabytes per second"," ");
    } else if (metric_info.unit == KB) {
        printf("%19s in kilobytes per second", " ");
    } else {
        printf("%19s in bytes per second", " ");
    }

    printf("         in Million ops/second%8sin microseconds\n", " ");

    /* hack */
    printf("shmem_add\n");
}

void static print_results_header(perf_metrics_t metric_info) {
    printf("\nResults for %d PEs %lu trials with window size %lu "\
            "max message size %lu with multiple of %lu increments, "\
            "\ntargeting %d remote PEs initiated from %d PEs\n", metric_info.num_pes,
            metric_info.trials, metric_info.window_size, metric_info.max_len,
            metric_info.size_inc, metric_info.sztarget, metric_info.szinitiator);
    printf("\nLength           %s           "\
            "Message Rate\n", metric_info.bw_type);

    printf("in bytes         ");
    if (metric_info.unit == MB) {
        printf("in megabytes per second");
    } else if (metric_info.unit == KB) {
        printf("in kilobytes per second");
    } else {
        printf("in bytes per second");
    }

    printf("         in messages/seconds\n");
}

void static print_data_results(double bw, double mr, perf_metrics_t data,
                            int len, double total_t) {
    static int atomic_type_index = 0;

    if(data.target_data) {
        if(data.my_node < data.midpt) {
            printf("initiator:\n");
        } else  {
            printf("target:\n");
        }
    }

    if (data.bwstyle == STYLE_ATOMIC) {
        printf("%-10s       ", dt_names[atomic_type_index]);
        atomic_type_index = (atomic_type_index + 1) % ATOMICS_N_DTs;
    } else
        printf("%9d       ", len);

    if(data.unit == KB) {
        bw = bw * 1e3;
    } else if(data.unit == B) {
        bw = bw * 1e6;
    }

    if (data.bwstyle == STYLE_ATOMIC) {
        printf("%5s%10.2f                        %10.2f%14s%10.2f\n", " ", bw,
                 mr/1e6, " ", total_t/(data.trials * data.window_size));
    } else
        printf("%10.2f                          %10.2f\n", bw, mr);
}


/* reduction to collect performance results from PE set
    then start_pe will print results --- assumes num_pes is even */
void static inline PE_set_used_adjustments(int *nPEs, int *stride, int *start_pe,
                                            perf_metrics_t my_info)
{
    red_PE_set PE_set = validation_set(my_info, nPEs);

    if(PE_set == FIRST_HALF || PE_set == FULL_SET) {
        *start_pe = 0;
    }
    else {
        assert(PE_set == SECOND_HALF);
        *start_pe = my_info.midpt;
    }

    *stride = 0; /* back to back PEs */
}


void static inline calc_and_print_results(double total_t, int len,
                            perf_metrics_t metric_info)
{
    int stride = 0, start_pe = 0, nPEs = 0;
    static double pe_bw_sum, bw = 0.0; /*must be symmetric for reduction*/
    double pe_bw_avg = 0.0, pe_mr_avg = 0.0;
    int nred_elements = 1;
    static double pwrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];

    PE_set_used_adjustments(&nPEs, &stride, &start_pe, metric_info);

    if (total_t > 0 ) {
        bw = (len / 1e6 * metric_info.window_size * metric_info.trials) /
                (total_t / 1e6);
    }

    /* 2x as many messages/bytes at once for bi-directional */
    if(metric_info.type == BI_DIR)
        bw *= 2;

    /* base case: will be overwritten by collective if num_pes > 2 */
    pe_bw_sum = bw;

    if(nPEs >= 2)
        shmem_double_sum_to_all(&pe_bw_sum, &bw, nred_elements, start_pe,
                                stride, nPEs, pwrk,
                                red_psync);

    /* aggregate bw since bw op pairs are communicating simultaneously */
    if(metric_info.my_node == start_pe) {
        pe_bw_avg = pe_bw_sum;
        pe_mr_avg = pe_bw_avg / (len / 1e6);
        print_data_results(pe_bw_avg, pe_mr_avg, metric_info, len, total_t);
    }
}

void static inline large_message_metric_chg(perf_metrics_t *metric_info, int len) {
    if(len > LARGE_MESSAGE_SIZE) {
        metric_info->window_size = WINDOW_SIZE_LARGE;
        metric_info->trials = TRIALS_LARGE;
        metric_info->warmup = WARMUP_LARGE;
    }
}

static void validate_atomics(perf_metrics_t m_info) {
    int snode = streaming_node(m_info);
    int * my_buf = (int *)m_info.dest;
    bw_type tbw = m_info.type;
    unsigned int expected_val = 0;
    unsigned int ppe_exp_val = ((m_info.trials + m_info.warmup) * m_info.window_size
                                * ATOMICS_N_DTs * ATOMICS_N_OPs) + m_info.my_node;

    if(m_info.cstyle == COMM_INCAST) {
        if(tbw == BI_DIR)
            printf("WARNING: This use-case is not currently well defined\n");

        if(m_info.my_node == 0) {
            expected_val = ppe_exp_val * m_info.num_pes;
        } else
            expected_val = m_info.my_node;
    } else {
        assert(m_info.cstyle == COMM_PAIRWISE);
        expected_val = ppe_exp_val;
    }

    if((!snode && tbw == UNI_DIR) || tbw == BI_DIR) {
        if(my_buf[0] != expected_val)
            printf("validation error for PE %d: %d != %d \n", m_info.my_node, my_buf[0],
                    expected_val);
    }
}

/**************************************************************/
/*                   Bi-Directional BW                        */
/**************************************************************/

/*have two symmetric char array metric_info->src/dest of max_len to
 * use for calculation initalized with my_node number
 * NOTE: post function validation assumptions, data isn't flushed pre/post */
extern void bi_dir_bw(int len, perf_metrics_t *metric_info);

void static inline bi_dir_bw_test_and_output(perf_metrics_t metric_info) {
    int len = 0, partner_pe = partner_node(metric_info);

    for (len = metric_info.start_len; len <= metric_info.max_len;
        len *= metric_info.size_inc) {

        large_message_metric_chg(&metric_info, len);

        bi_dir_bw(len, &metric_info);
    }

    shmem_barrier_all();

    if(metric_info.validate) {
        if(metric_info.bwstyle != STYLE_ATOMIC) {
            validate_recv(metric_info.dest, metric_info.max_len, partner_pe);
        } else {
            validate_atomics(metric_info);
        }
    }
}

/**************************************************************/
/*                   UNI-Directional BW                       */
/**************************************************************/

/*have one symmetric char array metric_info->buf of max_len to use for
 * calculation initalized with my_node number
 * NOTE: post function validation assumptions, data isn't flushed pre/post */
extern void uni_dir_bw(int len, perf_metrics_t *metric_info);

void static inline uni_dir_bw_test_and_output(perf_metrics_t metric_info) {
    int len = 0, partner_pe = partner_node(metric_info);

    if(metric_info.my_node == 0) {
        if (metric_info.bwstyle == STYLE_ATOMIC)
            print_atomic_results_header(metric_info);
        else
            print_results_header(metric_info);
    }

    for (len = metric_info.start_len; len <= metric_info.max_len;
        len *= metric_info.size_inc) {

        large_message_metric_chg(&metric_info, len);

        uni_dir_bw(len, &metric_info);
    }

    shmem_barrier_all();

    if(metric_info.validate) {
        if((streaming_node(metric_info) && metric_info.bwstyle == STYLE_GET) ||
            (target_node(metric_info) && metric_info.bwstyle == STYLE_PUT)) {
            validate_recv(metric_info.dest, metric_info.max_len, partner_pe);
        } else if(metric_info.bwstyle == STYLE_ATOMIC) {
            validate_atomics(metric_info);
        }
    }
}

/**************************************************************/
/*                   INIT and teardown of resources           */
/**************************************************************/

/*create and init (with my_PE_num) two symmetric arrays on the heap */
void static inline bw_init_data_stream(perf_metrics_t *metric_info,
                                            int argc, char *argv[]) {

    int i = 0;

    /*must be before data_init*/
#ifndef VERSION_1_0
    shmem_init();
#else
    start_pes(0);
#endif

    data_init(metric_info);

    for(i = 0; i < SHMEM_REDUCE_MIN_WRKDATA_SIZE; i++)
        red_psync[i] = SHMEM_SYNC_VALUE;

    command_line_arg_check(argc, argv, metric_info);

    metric_info->src = aligned_buffer_alloc(metric_info->max_len);
    init_array(metric_info->src, metric_info->max_len, metric_info->my_node);

    metric_info->dest = aligned_buffer_alloc(metric_info->max_len);
    init_array(metric_info->dest, metric_info->max_len, metric_info->my_node);
}


void static inline bi_dir_init(perf_metrics_t *metric_info, int argc,
                                char *argv[]) {
    bw_init_data_stream(metric_info, argc, argv);

    only_even_PEs_check(metric_info->my_node, metric_info->num_pes);

    bi_dir_data_init(metric_info);

}

void static inline uni_dir_init(perf_metrics_t *metric_info, int argc,
                                char *argv[], bw_style bwstyl) {
    bw_init_data_stream(metric_info, argc, argv);

    /* uni-dir validate needs to know if its a put or get */
    metric_info->bwstyle = bwstyl;

    if(metric_info->num_pes != 1)
        only_even_PEs_check(metric_info->my_node, metric_info->num_pes);

    uni_dir_data_init(metric_info);
}

void static inline bw_data_free(perf_metrics_t *metric_info) {
    shmem_barrier_all();

    aligned_buffer_free(metric_info->src);
    aligned_buffer_free(metric_info->dest);

#ifndef VERSION_1_0
    shmem_finalize();
#endif
}

void static inline bi_dir_bw_main(int argc, char *argv[]) {

    perf_metrics_t metric_info;

    bi_dir_init(&metric_info, argc, argv);

    bi_dir_bw_test_and_output(metric_info);

    bw_data_free(&metric_info);

} /*main() */

void static inline uni_dir_bw_main(int argc, char *argv[], bw_style bwstyl) {

    perf_metrics_t metric_info;

    uni_dir_init(&metric_info, argc, argv, bwstyl);

    uni_dir_bw_test_and_output(metric_info);

    bw_data_free(&metric_info);

} /*main() */
