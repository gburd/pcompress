/*
 * Sem_Wait() must keep waiting when a handled signal interrupts it.
 *
 * pcompress's threads hand work to each other through Sem_t pairs and none
 * of the twenty call sites checks Sem_Wait()'s result, so a wait that ends
 * on EINTR makes the waiter run on as if the other side had posted. A
 * program that links libpcompress and installs signal handlers (a daemon,
 * for example) delivers exactly such signals.
 */
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utils.h>
#include "minunit.h"

static Sem_t sem;
static volatile int woke;		/* set by the waiter once Sem_Wait returns */
static volatile sig_atomic_t hits;	/* signals the waiter has handled */

static void
on_usr1(int signo)
{
	(void) signo;
	hits++;
}

static void *
waiter(void *arg)
{
	(void) arg;
	Sem_Wait(&sem);
	__atomic_store_n(&woke, 1, __ATOMIC_RELEASE);
	return (NULL);
}

static void
nap(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

MU_TEST(test_sem_wait_survives_signals)
{
	struct sigaction sa;
	pthread_t t;
	int i;

	memset(&sa, 0, sizeof (sa));
	sa.sa_handler = on_usr1;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* no SA_RESTART: sem_wait() is not restarted anyway */
	mu_assert_int_eq(0, sigaction(SIGUSR1, &sa, NULL));
	mu_assert_int_eq(0, Sem_Init(&sem, 0, 0));
	mu_assert_int_eq(0, pthread_create(&t, NULL, waiter, NULL));

	/* Interrupt the blocked waiter repeatedly; it must not wake up. */
	for (i = 0; i < 20; i++) {
		nap(10);
		pthread_kill(t, SIGUSR1);
	}
	nap(50);
	mu_assert(hits > 0, "the signal never reached the waiting thread");
	mu_assert(!__atomic_load_n(&woke, __ATOMIC_ACQUIRE),
	    "Sem_Wait returned on a signal without a post");

	/* A real post still wakes it. */
	Sem_Post(&sem);
	pthread_join(t, NULL);
	mu_assert(__atomic_load_n(&woke, __ATOMIC_ACQUIRE), "Sem_Post did not wake the waiter");
	Sem_Destroy(&sem);
}

MU_TEST_SUITE(suite_sem)
{
	MU_RUN_TEST(test_sem_wait_survives_signals);
}

int
main(void)
{
	MU_RUN_SUITE(suite_sem);
	MU_REPORT();
	return (MU_EXIT_CODE);
}
