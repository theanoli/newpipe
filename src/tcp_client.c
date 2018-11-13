#include "harness.h"

#include <sys/epoll.h>


// Initialize protocol-specific data structures
void
Init (ProgramArgs *p, int *pargc, char ***pargv)
{
    ThreadArgs *targs = (ThreadArgs *)calloc (p->nthreads, 
            sizeof (ThreadArgs));

    if (targs == NULL) {
        printf ("Error malloc'ing space for thread args!\n");
        exit (-10);
    }
    p->thread_data = targs;

    p->tids = (pthread_t *)calloc (p->nthreads, sizeof (pthread_t));
    if (p->tids == NULL) {
        printf ("Failed to malloc space for tids!\n");
        exit (-82);
    }
}


void
LaunchThreads (ProgramArgs *p)
{
    int i, ret;
    cpu_set_t cpuset __attribute__((__unused__));

    ThreadArgs *targs = p->thread_data;

    for (i = 0; i < p->nthreads; i++) {
        targs[i].machineid = p->machineid;
        targs[i].threadid = i;
        snprintf (targs[i].threadname, 128, "[%s.%d]", p->machineid, i);

        if (p->tr) {
            // E.g., if there are 8 server threads but only 4 cores, and 16
            // client threads, then want to cycle through portnos 8000-8003 only.
            // If there are 2 server threads with 4 cores, and 16 client threads, 
            // cycle through portnos 8000-8001 only.
            targs[i].port = p->port + i % p->ncli % NSERVERCORES; 
        } else {
            targs[i].port = p->port + i % NSERVERCORES;
        }
        targs[i].host = p->host;
        targs[i].tr = p->tr;
        targs[i].nrtts = p->nrtts;
        targs[i].online_wait = p->online_wait;
        targs[i].latency = p->latency;
        targs[i].ncli = p->ncli;
        targs[i].ep = -1;
        memcpy (targs[i].sbuff, p->sbuff, PSIZE + 1);

        if (args.no_record) {
            targs[i].outfile = NULL;
        } else
            setup_filenames (&targs[i]);
        }

        printf ("[%s] Launching thread %d, ", p->machineid, i);
        printf ("connecting to portno %d\n", targs[i].port);

        pthread_create (&p->tids[i], NULL, ThreadEntry, (void *)&targs[i]);
        
        // Always pin cores
        // This is fragile; better to get available cores programmatically
        // instead of using hardcoded macro value NSERVERCORES
        CPU_ZERO (&cpuset);
        CPU_SET (i % NSERVERCORES, &cpuset);
        ret = pthread_setaffinity_np (p->tids[i], sizeof (cpu_set_t), &cpuset);
        if (ret != 0) {
            printf ("[%s] Couldn't pin thread %d to core!\n", p->machineid, i);
            exit (-14);
        }
    }
}
