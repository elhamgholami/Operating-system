#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

void removeThreadFromQueue(struct proc *parent, struct proc *thread) {
  
  for(int i = 0; i < parent->thread_count; i++) {
    if(parent->thread_queue[i] == thread) {
      // Shift remaining threads in the queue
      for(int j = i; j < parent->thread_count - 1; j++) {
        parent->thread_queue[j] = parent->thread_queue[j + 1];
      }
      parent->thread_count--;
      break;
    }
  }
}





// void scheduleThreadsRoundRobin(struct proc *p, struct cpu *c) {
//     int idx = p->next_thread;

//     for(int i = 0; i < p->thread_count; i++) {
//         struct proc *tp = p->thread_queue[(idx + i) % p->thread_count];
        
//         cprintf("%d\n", i);
//         if(tp->state == RUNNABLE) {
//             // Schedule thread
//             c->proc = tp;
//             switchuvm(tp);
//             tp->state = RUNNING;
//             swtch(&(c->scheduler), tp->context);
//             switchkvm();

//             c->proc = 0;
//             p->next_thread = (idx + i + 1) % p->thread_count;
            
//             break;
//         }
//     }
// }





int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
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


  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  // p->thread_count = 1; //Elham

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
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
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
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

int 
clone(void(*fcn)(void*,void*), void *arg1, void *arg2, void* stack)
{
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // p->thread_count++; //Elham

  // Copy process data to the new thread
  np->pgdir = p->pgdir;
  np->sz = p->sz;
  np->parent = p;
  *np->tf = *p->tf;
  
  void * sarg1, *sarg2, *sret;

  // Push fake return address to the stack of thread
  sret = stack + PGSIZE - 3 * sizeof(void *);
  *(uint*)sret = 0xFFFFFFF;

  // Push first argument to the stack of thread
  sarg1 = stack + PGSIZE - 2 * sizeof(void *);
  *(uint*)sarg1 = (uint)arg1;

  // Push second argument to the stack of thread
  sarg2 = stack + PGSIZE - 1 * sizeof(void *);
  *(uint*)sarg2 = (uint)arg2;

  // Put address of new stack in the stack pointer (ESP)
  np->tf->esp = (uint) stack;

  // Save address of stack
  np->threadstack = stack;

  // Initialize stack pointer to appropriate address
  np->tf->esp += PGSIZE - 3 * sizeof(void*);
  np->tf->ebp = np->tf->esp;

  // Set instruction pointer to given function
  np->tf->eip = (uint) fcn;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  int i;
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
 
  acquire(&ptable.lock);

  if(p->thread_count < MAX_THREADS)
  {
    p->thread_queue[p->thread_count] = np;
    p->thread_count++;
    np->state = RUNNABLE;
    

  }
  else
  {
    kfree(np->kstack);
    np->kstack=0;
    np->state = UNUSED;
    release(&ptable.lock);
    return -1;
    
  }
  




  release(&ptable.lock);

  return np->pid;
}

int
join(void **stack) {
  struct proc *p;
  int havekids, pid;
  struct proc *cp = myproc();

  acquire(&ptable.lock);
  for(;;) {
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if(p->parent != cp || p->pgdir != p->parent->pgdir)
        continue;

      havekids = 1;
      if(p->state == ZOMBIE) {
        // Found one.
        pid = p->pid;
        *stack = p->threadstack;
        kfree(p->kstack);
        p->kstack = 0;
        removeThreadFromQueue(cp, p);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->threadstack = 0;

        // Remove the thread from the parent's thread queue

        release(&ptable.lock);
        return pid;
      }
    }

    if(!havekids || cp->killed) {
      release(&ptable.lock);
      return -1;
    }

    sleep(&ptable.lock);
  }
}



// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
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
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
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


// void scheduler(void) {
//   struct proc *p;
//   struct cpu *c = mycpu();
//   c->proc = 0;

//   for(;;){
//     // Enable interrupts on this processor.
//     sti();

//     acquire(&ptable.lock);
//     for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//       if(p->state != RUNNABLE)
//         continue;

//       // Check if the process has any threads
//       if(p->thread_count > 0){
//         // Round-robin scheduling among the threads
//         struct proc *thread_to_run;
//         int i, found = 0;

//         // Find the next runnable thread in the queue
//         for(i = 0; i < p->thread_count; i++) {
//           int idx = (p->next_thread + i) % p->thread_count;
//           if(p->thread_queue[idx]->state == RUNNABLE) {
//             thread_to_run = p->thread_queue[idx];
//             p->next_thread = (idx + 1) % p->thread_count;
//             found = 1;
//             break;
//           }
//         }

//         if(found) {
//           c->proc = thread_to_run;
//           switchuvm(thread_to_run);
//           thread_to_run->state = RUNNING;
//           swtch(&(c->scheduler), thread_to_run->context);
//           switchkvm();
//           c->proc = 0;
//         }
//       } else {
//         // Schedule the process itself
//         c->proc = p;
//         switchuvm(p);
//         p->state = RUNNING;
//         swtch(&(c->scheduler), p->context);
//         switchkvm();
//         c->proc = 0;
//       }
//     }
//     release(&ptable.lock);
//   }
// }


void scheduler(void) {
    struct cpu *c = mycpu();
    c->proc = 0;

    for(;;) {
        sti();

        acquire(&ptable.lock);
        for(struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            
            if(p->state != RUNNABLE)
                continue;
            else if(p->thread_count > 0) {
              
                // Get the next thread to run from the thread queue
                struct proc *t = p->thread_queue[p->next_thread];
                // Increment the next thread index modulo the thread count
                p->next_thread = (p->next_thread + 1) % p->thread_count;
                // Check if the thread is runnable
                if(t->state == RUNNABLE) {
                    // Schedule thread
                    c->proc = t;
                    switchuvm(t);
                    t->state = RUNNING;
                    swtch(&(c->scheduler), t->context);
                    switchkvm();
                    c->proc = 0;
                
            } 
            }
            
            else {
                // Schedule process
                c->proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&(c->scheduler), p->context);
                switchkvm();
                c->proc = 0;
            }
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
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
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
  struct proc *p = myproc();
  
  if(p == 0)
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
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

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
