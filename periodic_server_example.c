// periodic_server_example.c
// gcc -O2 -Wall periodic_server_example.c -o periodic_server_example -pthread
// sudo ./periodic_server_example
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define NS_PER_SEC 1000000000L
#define NS_PER_MS  1000000L

/////////////////////// FILA DE REQUISIÇÕES APERIÓDICAS ///////////////////////
typedef void (*job_func_t)(void *arg);

typedef struct job {
    job_func_t func;
    void *arg;
    struct job *next;
} job_t;

static job_t *queue_head = NULL;
static job_t *queue_tail = NULL;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

void enqueue_job(job_func_t f, void *arg) {
    job_t *j = malloc(sizeof(job_t));
    j->func = f;
    j->arg  = arg;
    j->next = NULL;

    pthread_mutex_lock(&queue_mutex);
    if (queue_tail)
        queue_tail->next = j;
    else
        queue_head = j;
    queue_tail = j;
    pthread_mutex_unlock(&queue_mutex);
}

job_t *dequeue_job(void) {
    job_t *j = queue_head;
    if (!j) return NULL;
    queue_head = j->next;
    if (!queue_head) queue_tail = NULL;
    return j;
}

/////////////////////// UTILITÁRIOS DE TEMPO ///////////////////////
static void timespec_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= NS_PER_SEC) {
        t->tv_nsec -= NS_PER_SEC;
        t->tv_sec  += 1;
    }
}

static long timespec_diff_ns(const struct timespec *end,
                             const struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * NS_PER_SEC +
           (end->tv_nsec - start->tv_nsec);
}

/////////////////////// Pequeno "trabalho" de 5ms para simular carga
static void busy_5ms(void) {
    struct timespec req = {0};
    req.tv_nsec = 5 * NS_PER_MS;
    nanosleep(&req, NULL);
}

/////////////////////// JOB APERIÓDICO ///////////////////////
void aperiodic_job(void *arg) {
    int id = *(int *)arg;
    printf("[SERVIDOR] Executando job aperiódico %d\n", id);
    busy_5ms();  // simula tempo de processamento
    free(arg);
}

/////////////////////// SERVIDOR PERIÓDICO ///////////////////////
typedef struct {
    long period_ns;  // Ts
    long budget_ns;  // Cs
} server_params_t;

void *server_thread(void *arg) {
    server_params_t *params = (server_params_t *)arg;
    long Ts = params->period_ns;
    long Cs = params->budget_ns;

    struct timespec next_release;
    clock_gettime(CLOCK_MONOTONIC, &next_release);

    while (1) {
        timespec_add_ns(&next_release, Ts);   // próxima liberação
        struct timespec start_period;
        clock_gettime(CLOCK_MONOTONIC, &start_period);

        long elapsed = 0;

        while (elapsed < Cs) {
            pthread_mutex_lock(&queue_mutex);
            job_t *j = dequeue_job();
            pthread_mutex_unlock(&queue_mutex);

            if (!j) {
                // nada para fazer neste período
                break;
            }

            struct timespec t_before, t_after;
            clock_gettime(CLOCK_MONOTONIC, &t_before);
            j->func(j->arg);    // executa job
            clock_gettime(CLOCK_MONOTONIC, &t_after);

            elapsed = timespec_diff_ns(&t_after, &start_period);
            free(j);
        }

        // Dorme até o próximo período (tempo absoluto)
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                        &next_release, NULL);
    }
    return NULL;
}

/////////////////////// TAREFAS PERIÓDICAS (T1..T4) ///////////////////////
typedef struct {
    const char *name;
    long period_ns;
} periodic_task_params_t;

void *periodic_task(void *arg) {
    periodic_task_params_t *p = (periodic_task_params_t *)arg;
    struct timespec next_release;
    clock_gettime(CLOCK_MONOTONIC, &next_release);

    while (1) {
        timespec_add_ns(&next_release, p->period_ns);
        printf("[%s] executando\n", p->name);
        busy_5ms();  // cada tarefa periódica consome ~5ms
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                        &next_release, NULL);
    }
    return NULL;
}

/////////////////////// GERADOR DE REQUISIÇÕES APERIÓDICAS ///////////////////////
void *aperiodic_generator(void *arg) {
    (void)arg;
    int counter = 0;
    while (1) {
        // gera um job a cada 30–120 ms
        int delay_ms = 30 + rand() % 90;
        struct timespec req = {0};
        req.tv_nsec = delay_ms * NS_PER_MS;
        nanosleep(&req, NULL);

        int *id = malloc(sizeof(int));
        *id = ++counter;
        printf("[GERADOR] nova requisição aperiódica %d\n", *id);
        enqueue_job(aperiodic_job, id);
    }
    return NULL;
}

/////////////////////// MAIN ///////////////////////
static void create_rt_thread(pthread_t *th,
                             void *(*func)(void*),
                             void *arg,
                             int priority)
{
    pthread_attr_t attr;
    struct sched_param sp;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = priority;
    pthread_attr_setschedparam(&attr, &sp);

    int ret = pthread_create(th, &attr, func, arg);
    if (ret != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(ret));
    }
    pthread_attr_destroy(&attr);
}

int main(void) {
    srand(time(NULL));

    // ----------------- Servidor periódico -----------------
    server_params_t server_params = {
        .period_ns = 50 * NS_PER_MS,   // Ts = 50ms
        .budget_ns = 20 * NS_PER_MS    // Cs = 20ms por período
    };
    pthread_t th_server;
    create_rt_thread(&th_server, server_thread, &server_params, 60);

    // ----------------- Tarefas periódicas -----------------
    periodic_task_params_t tparams[4] = {
        {"T1", 40 * NS_PER_MS},
        {"T2", 80 * NS_PER_MS},
        {"T3", 120 * NS_PER_MS},
        {"T4", 160 * NS_PER_MS}
    };

    pthread_t th_tasks[4];
    for (int i = 0; i < 4; ++i) {
        create_rt_thread(&th_tasks[i], periodic_task, &tparams[i], 50);
    }

    // ----------------- Gerador aperiódico -----------------
    pthread_t th_gen;
    // gerador pode ter prioridade menor (não precisa ser RT)
    pthread_create(&th_gen, NULL, aperiodic_generator, NULL);

    printf("Rodando por 10 segundos...\n");
    sleep(10);

    printf("Encerrando (para exemplo simples vamos apenas sair do processo).\n");
    // Em código real você sinalizaria as threads para sair, etc.
    return 0;
}
