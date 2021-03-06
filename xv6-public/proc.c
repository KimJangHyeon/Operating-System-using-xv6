#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

/* VARIABLE */
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

int boost_check = 0;
// This variable is for checking whether it's priority boost time. Actually it's total ticks.

int total_tickets = 0;
// This variable is representing total_tickets(Maximum value is 80).

int mlfq_pass_value = 0;
// This variable is used to decide which scheduler to use

int stride_pass_value = 0;
// This variable is used to decide which scheduler to use

/* FUNCTION */
void priority_manage(struct proc *p);
/* This function adjusts process's priority when it has used its all time slice.
 * @param[struct proc *p]       P is the process which will be managed.
 */
int getlev(void);
/* This function returns current process' priority.
 */
int set_cpu_share(int share);
/* This function gets cpu share when it's available situation.(when total tickets are less than 80)
 * @param[int share]        Share is the tickets that user inputs.
 * @return                  returns amount of tickets.
 */
void add_clock(void);
/* This function adds time clock(I mean entire time clock. Not process' time clock),
 * and do priority boost at proper time(When total time clock == 100)
 */
void stride_realloc(void);
/* This function adjusts process' stride when total tickets have been changed.
 */
int decide_scheduler(void);
/* This function decides which scheduler to use.
 * @return      1 if Stride Scheduler is selected, 0 if MLFQ Scheduler is selected.
 */

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  int i;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
        // Execption handling : When highest queue is full.
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  // Thread ID initialize.
  p->tid = -1;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);

  p->context->eip = (uint)forkret;

  // /* Initialize process' attributes */ //

  p->priority = 0;
  // Priority level starts from 0. 0 is the Highest level, 2 is the lowest level.

  p->ticks = 0;
  // Initialize ticks. It'll be used to record use of each process's time slices.

  /* At first, you don't know whether this process will be scheduled in Stride Scheduling.
     So initialize to 0. (When cpu_set_share is called, this will change those attributes.) */
  p->tickets = 0;
  p->stride = 0;
  p->pass_value = 0;
  
  // Initialize tspace.
  for(i = 0; i < 10; i++){
        p->tspace[i] = 0;      
  }

  return p;
}
//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;

  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;

  // Thread calls growproc
  if(proc->tid > 0){
    sz = proc->parent->sz;
    if(n > 0){
        if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
            return -1;
    } else if(n < 0){
        if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
            return -1;
    }
    proc->parent->sz = sz;
  }
  else{
    sz = proc->sz;
    if(n > 0){
        if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
            return -1;
    } else if(n < 0){
        if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
            return -1;
    }
    proc->sz = sz;
  }
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // When thread calls fork
  if(proc->tid > 0){
    // Copy process state from p.
    if((np->pgdir = copyuvm(proc->pgdir, proc->parent->sz)) == 0){
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        return -1;
    } 
    np->std = proc->parent->std;
    np->sz = proc->parent->sz;
  }
  // When process calls fork
  else{
    // Copy process state from p.
    if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        return -1;
    }
    np->std = proc->std;
    np->sz = proc->sz;
  }
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  pid = np->pid;
  acquire(&ptable.lock);
  
  np->state = RUNNABLE;
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc->parent == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
        fileclose(proc->ofile[fd]);
        proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  // When thread calls exit().
  if(proc->tid > 0){
    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(proc->parent);

    // Pass abandoned children to init.
    // Change all threads' state that process has.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->parent == proc && p->tid == -1){
            p->parent = initproc;
        if(p->state == ZOMBIE)
            wakeup1(initproc);
        }
    }
    // Kill thread's parent(Process)
    proc->parent->killed = 1;
  }
  // When process calls exit().
  else{
    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(proc->parent);

    // Pass abandoned children to init.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->parent == proc && p->tid > 0){
            p->killed = 1;

            if(p->state == SLEEPING){
                p->state = RUNNABLE;
                continue;
            }
        }
        if(p->parent == proc && p->tid == -1){
            p->parent = initproc;
            if(p->state == ZOMBIE)
                wakeup1(initproc);
        }
    }
  }
  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;

  /* Adjust current process'(This will be terminated) attributes. */
  if(proc->tickets > 0){
    total_tickets -= proc->tickets;
    // Change total tickets.

    proc->tickets = 0;
    proc->stride = 0;
    proc->pass_value = 0;

    /* Total tickets has been changed, so call stride_realloc(). */
    if(total_tickets > 0){
        stride_realloc();
    }   
  }

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids,pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent->state == ZOMBIE && p->state == ZOMBIE && p->tid > 0){
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
      }
    }
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
    struct proc *p; 
    struct proc ref;
    int level, ps_val, i;

    for(;;){
        level = 2;
        ps_val = 0;

        sti();

        /* In scheduler function, first it decides which scheduelr to use.
           Function decide_scheduler() returns 1 when it comes to use Stride Scheduler.
           It returns 0 when it comes to use MLFQ Scheduler. */

        /* Stride Scheduling */
        if(decide_scheduler()){
            acquire(&ptable.lock);

            for(i = 0, ref = ptable.proc[i]; i < NPROC; ref = ptable.proc[i++]){
                if(ref.state == RUNNABLE && ref.tickets != 0){
                    ps_val = ref.pass_value;
                    break;
                }
            } 
            // Search reference pass value. 
            
            for(i = 0, ref = ptable.proc[i]; i < NPROC; ref = ptable.proc[i++]){
                if(ref.state == RUNNABLE && ref.tickets != 0 && ref.pass_value < ps_val){
                    ps_val = ref.pass_value;
                }
            }
            // Decide pass value. Pass value must be the least one.

            for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
                if(p->state != RUNNABLE || p->pass_value != ps_val){
                    continue;
                }

                p->pass_value += p->stride;
                // Manage pass value.

                proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&cpu->scheduler, p->context);
                switchkvm();
                // Process is done running for now.
                // It should have changed its p->state before coming back.
                proc = 0;
            }
            // Search selected process, then schedule it.
        }
        
        /* MLFQ Scheduling */
        else{
            acquire(&ptable.lock);

            for(i = 0, ref = ptable.proc[i]; i < NPROC; ref = ptable.proc[i++]){
                if(ref.state == RUNNABLE && ref.tickets == 0  && ref.priority < level){
                    level = ref.priority;
                }
            }
            // Decide process' priority. It  must be the least one. 

            for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
                if(p->state != RUNNABLE || p->tickets != 0 ||p->priority != level){
                    continue;
                }
                proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&cpu->scheduler, p->context);
                switchkvm();
                // Process is done running for now.
                // It should have changed its p->state before coming back.
                proc = 0;
            }
            // Search selected process, then schedule it.
        }

        release(&ptable.lock);
    }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(int timer_interrupt)
{
  /* If yield is called from timer_interrupt,
     you must manage process' time clock. */

  if(timer_interrupt){
    proc->ticks++;
    priority_manage(proc);
    // Add process' time clock, then manage it.
  }

  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
        p->killed = 1;
        // Wake process from sleep if necessary.
        if(p->state == SLEEPING)
            p->state = RUNNABLE;
        release(&ptable.lock);
        return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

/* This function manages argument process's priority. */
void
priority_manage(struct proc *p)
{
  switch (p->priority) {
      case 0 :
          if(p->ticks == 5){
              p->priority++;        
              p->ticks = 0;
          }
          // In case of priority 0 -> 1
      break;

      case 1 :
          if(p->ticks == 10){
              p->priority++;        
              p->ticks = 0;
          }
          // In case of priority 1 -> 2
      break;
 
      case 2 :
          if(p->ticks == 20){
              p->ticks = 0;
          }
          //In case of priority 2 -> 2
      break;
  }
}

/* This function returns current process' priority. */
int
getlev(void)
{
    return proc->priority;
}

/* This function set process' tickets, stride, and initialize pass value. */
int
set_cpu_share(int share)
{
    if(share <= 0){

        return -1;           
    }
    // When argument is below 0, returns -1(Error).

    else{
        if(total_tickets + share <= 80){
        // First, check if total tickets become 80(Limitation).
            
            /* Set process' tickets, pass value, and reinitialize total tickets. */
            proc->tickets = share;
            total_tickets += share;
            proc->pass_value = 0;

            /* Reinitialize all process' stride. */
            acquire(&ptable.lock);
            stride_realloc();
            release(&ptable.lock);

            return share;
        }
        else{
            return -1;
        }
        // When total tickets become over 80, block the input.
    }
    // When argument is above 0.
}

/* This function adds total clock, and boosts priority when it needs. */
void
add_clock(void)
{
    boost_check++;
    
    /* When it comes to priority boost situation. */
    if(boost_check == 100){
        struct proc ref;
        int i;

        acquire(&ptable.lock);

        for(i = 0, ref = ptable.proc[i]; i < NPROC; ref = ptable.proc[i++]){
            ref.priority = 0;
            ref.ticks = 0;
        }
        // Reset all process' priority and time clock.
              
        release(&ptable.lock);

        boost_check = 0;

        /* Useless cod e*/
        i = ref.priority;
        // To avoid "unused but set problem".
    }
}

/* This function reinitialize all process' stride.
   When total tickets change, all process' stride must be changed. */
void
stride_realloc(void)
{
    struct proc *p;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->tickets > 0){
            p->stride = total_tickets / p->tickets;
            // Process' stride = Total tickets / Process' tickets
        }
    }
}

/* This function decides which scheduler to use.
   In this function, I apply Stride algorithm once more. */
int
decide_scheduler(void)
{
    int mlfq_stride, stride_stride;

    if(total_tickets){
    /* In every case total tickets are 100(CPU share 100%).
       Each Scheduler's stride = 100 / Scheduler's tickets.

       Stride Scheduler's tickets : Total tickets which used in 
                                    Stride Scheduler. (Limitation : 80)

       MLFQ Scheduler's tickests : 100 - Stride Scheduler's tickets. */

        stride_stride = 100 / total_tickets;
        mlfq_stride = 100 / (100 - total_tickets);
    }

    /* If there is no process which will be scheduled in Stride Scheduler,
       (This means total tickets are 0) select MLFQ Scheduler */
    else{
        return 0;
    }

    /* Select scheduler whose pass value is less than the other's. */

    if(stride_pass_value <= stride_pass_value){
        stride_pass_value += stride_stride;    
        return 1;
    }
    // When Stride Scheduler is selected.

    else{
        mlfq_pass_value += mlfq_stride;
        return 0;
    }
    // When MLFQ Scheduler is selected.
}
/* Thread Function */

/* This function creates thread with given argument(arg).
 * Then saves its thread id in given argument(thread).
 * @param[out]    thread           ID of new thread
 * @param[in]     start_routine    Thread function to execute(main function of new thread)
 * @param[in]     arg              argument pass to start_routine
 * return                          If souccess 0, else -1
 */
int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
    int i,espace = -1;

    struct proc *nt, *tparent;
    uint sp, ustack[2];

    /* kernel stack */

    // Allocate thread
    if((nt = allocproc()) == 0){
        return -1;
    }

    // Initialize thread ID.
    nt->tid = nexttid++;

    // When thread calls thread_create. Including nested case.
    tparent = proc;
    while(tparent->tid > 0){
        tparent = tparent->parent;
    }
    nt->parent = tparent;

    // Allocate tickets
    if(tparent->tickets){
        nt->tickets = tparent->tickets;
        nt->stride = tparent->stride;
        nt->pass_value = tparent->pass_value;
    }

    // Reallocate pid
    nt->pid = tparent->pid;
    nextpid--;

    nt->pgdir = tparent->pgdir;
    *nt->tf = *tparent->tf;
    // Initialize standard.    
    nt->std = tparent->std;

    for(i = 0; i < NOFILE; i++){
        if(proc->ofile[i]){
            nt->ofile[i] = filedup(proc->ofile[i]);
        }
    }
    nt->cwd = idup(proc->cwd);

    safestrcpy(nt->name, tparent->name, sizeof(tparent->name));

    /* User stack */

    // Find empty space
    for(i = 0; i < 10; i++){
        if(tparent->tspace[i] == 0){
            tparent->tspace[i] = 1;
            nt->tspace[i] = 1;
            espace = i;
            break;
        }
    }
    // When there are no empty spaces : error
    if(espace < 0){
        return -1;
    }

    if((sp = allocuvm(nt->pgdir, tparent->std  + (2 * PGSIZE) * (espace), tparent->std  + (2 * PGSIZE) * (espace) + 2 * PGSIZE))==0){   
        return -1;
    }
    clearpteu(nt->pgdir, (char*)(tparent->std  + (2 * PGSIZE) * (espace)));

    // Reallocate sz : In thread, sz means each thread's top stack loaction. In process, sz means size of whole process memory.
    nt->sz = sp;
    nt->parent->sz = nt->parent->std + 10 * (2*PGSIZE);

    ustack[0] = 0xffffffff;  // fake return PC
    ustack[1] = (uint)arg;

    // Decrease stack pointer(size of uint * 2)
    sp -= 8;
    if(copyout(nt->pgdir, sp, ustack, 8) < 0){
        return -1;
    }

    // Save start routine(Return address) in eip
    nt->tf->eip = (uint)(*start_routine);
    // Save new sp in esp
    nt->tf->esp = sp;
  
    // Save thread ID in given argument
    *thread = nt->tid;

    // Change new thread's state
    acquire(&ptable.lock);
    nt->state = RUNNABLE;
    release(&ptable.lock);

    return 0;
}

/* This function terminates current thread.
 * @param[in]     retval    Another thread which waits for the thread
 *                          by thread_join can get this value.
 */
void
thread_exit(void *retval)
{
//    int fd;

    // Save current thread's return value
    proc->ret_val = retval;

    if(proc == initproc)
        panic("init exiting");

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(proc->parent);

    // Change thread's state. 
    // Jump into the scheduler, never to return.
    proc->state = ZOMBIE;

    sched();
    panic("zombie exit");
}

/* This function waits for the termination of a specific thread.
 * @param[out]    thread           Thread ID of target thread which would terminate
 * @param[in]     arg              Return value of terminated thread
 * return                          If souccess 0, else -1
 */
int
thread_join(thread_t thread, void **retval)
{
    struct proc *p;
    int havethread,i;

    acquire(&ptable.lock);
    for(;;){
        // Scan through table looking for exited children.
        havethread = 0;
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
            if(p->tid != thread)
                continue;
            havethread = 1;
            if(p->state == ZOMBIE){
                // Found one.
                /* Reset attributes. */

                // Deallocate kernel stack.
                kfree(p->kstack);
                p->kstack = 0;

                p->pid = 0;
                p->tid = 0;

                // Store thread's return value.
                *retval = p->ret_val;
                p->ret_val = 0;
                
                for(i = 0; i < 10; i++){
                    if(p->tspace[i] == 1){
                        p->tspace[i] = 0;

                        // Remove parent's tspace also.
                        p->parent->tspace[i] = 0;

                        break;
                    }
                }

                // Deallocate user stack.
                deallocuvm(p->pgdir, p->sz, p->sz - 2*PGSIZE);

                p->sz = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->state = UNUSED;
                release(&ptable.lock);

                return 0;
            }
        }

        // No point waiting if it's not the chosen thread.
        if(!havethread || proc->killed){
            release(&ptable.lock);
            return -1;
        }

        // Wait for the chosen thread to exit.  (See wakeup1 call in proc_exit.)
        sleep(proc, &ptable.lock);  //DOC: wait-sleep
    }
}
