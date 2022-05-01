#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "list.h"
#include "proc.h"
#include "spinlock.h"
#include "scheduler.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct mlfq mlfq;
  struct stride stride;
  struct list_head sleep;
  struct list_head free;
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

static void pushheap(struct proc*);
static void enqueue(int, struct proc*);
static void dequeue(struct proc*);
static int getminpass(void);

// New functions

// inctick is used in sys_sleep
// It prevents gaming the scheduler.
void
inctick(void)
{
  acquire(&ptable.lock);
  myproc()->ticks++;
  release(&ptable.lock);
}

// set_cpu_share is used in sys_set_cpu_share
int
set_cpu_share(int share)
{
  struct proc *p;
  int remain;
  int minpass, mlfqpass;

  if(share < 1 || share > 100 - RESERVE)
    return -1;

  acquire(&ptable.lock);
  p = myproc();
  remain = ptable.mlfq.tickets;
  if(p->type == STRIDE)
    remain += p->tickets;
  if(remain - share >= RESERVE){
    if(p->type == MLFQ){
      dequeue(p);
      minpass = getminpass();
      mlfqpass = ptable.mlfq.pass;
      p->pass = minpass < mlfqpass ? minpass : mlfqpass;
      p->type = STRIDE;
      list_add(&p->queue, &ptable.stride.run);
    }
    ptable.mlfq.tickets = remain - share;
    p->tickets = share;
    release(&ptable.lock);
    return 0;
  } else {
    release(&ptable.lock);
    return -1;
  }
}

// getminpass is a function which returns
// the minimum pass value of stride heap.
static int
getminpass(void)
{
  return ptable.stride.size > 0 ?
    ptable.stride.minheap[1]->pass : MAXINT;
}

// pushheap is used for the stride scheduler.
// This heap is a min-heap for the pass of process.
// RUNNABLE, SLEEPING states are managed in it.
static void
pushheap(struct proc *p)
{
  int i = ++ptable.stride.size;
  struct proc **minheap = ptable.stride.minheap;

  while(i != 1 && p->pass < minheap[i/2]->pass){
    minheap[i] = minheap[i/2];
    i /= 2;
  }
  minheap[i] = p;
}

// pushheap is used for the stride scheduler.
static struct proc*
popheap()
{
  int parent, child;
  struct proc **minheap = ptable.stride.minheap;
  struct proc *min = minheap[1];
  struct proc *last = minheap[ptable.stride.size--];

  for(parent=1, child=2; child <= ptable.stride.size; parent=child, child*=2){
    if(child < ptable.stride.size && minheap[child]->pass > minheap[child+1]->pass) child++;
    if(last->pass <= minheap[child]->pass) break;
    minheap[parent] = minheap[child];
  }
  minheap[parent] = last;

  return min;
}

// enqueue pushes proc to MLFQ.
// In MLFQ, proc states are as following:
//   RUNNING, RUNNABLE
static void
enqueue(int level, struct proc *p)
{
  struct list_head *queue;

  queue = &ptable.mlfq.queue[level];
  list_add_tail(&p->queue, queue);
}

// concatqueue is used in MLFQ queues.
// It is a only way to move the process upward,
// and called when priority boost.
static void
concatqueue(int src, int dst)
{
  struct list_head *srcq = &ptable.mlfq.queue[src];
  struct list_head *dstq = &ptable.mlfq.queue[dst];
  struct list_head **spin = &ptable.mlfq.pin[src];
  struct list_head **dpin = &ptable.mlfq.pin[dst];

  if(list_empty(dstq) && *spin != srcq)
    *dpin = *spin;
  *spin = srcq;

  list_bulk_move_tail(srcq, dstq);
}

// dequeue pops proc from MLFQ.
// In MLFQ, proc states are as following:
//   RUNNING, RUNNABLE
static void
dequeue(struct proc *p)
{
  struct list_head **ppin;
  struct list_head *itr;

  ppin = &ptable.mlfq.pin[p->privlevel];
  itr = &p->queue;

  if(*ppin == itr)
    *ppin = itr->next;
  list_del(&p->queue);
}
// New functions end

void
pinit(void)
{
  struct proc *p;
  int i;

  initlock(&ptable.lock, "ptable");

  for(i = 0; i < QSIZE; i++){
    list_head_init(&ptable.mlfq.queue[i]);
    ptable.mlfq.pin[i] = &ptable.mlfq.queue[i];
  }
  list_head_init(&ptable.stride.run);
  list_head_init(&ptable.sleep);
  list_head_init(&ptable.free);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    list_add_tail(&p->queue, &ptable.free);

  ptable.mlfq.tickets = 100;
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

  if(!list_empty(&ptable.free)){
    p = list_first_entry(&ptable.free, struct proc, queue);
    list_del(&p->queue);
    goto found;
  }

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  list_head_init(&p->children);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    list_add(&p->queue, &ptable.free);
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
  enqueue(p->privlevel, p);

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
    list_add(&np->queue, &ptable.free);
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  list_add_tail(&np->sibling, &curproc->children);

  *np->tf = *curproc->tf;
  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  np->type = MLFQ;

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  enqueue(np->privlevel, np);

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p, *curproc = myproc();
  struct list_head *children, *itr;
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
  children = &curproc->children;
  for(itr = children->next; itr != children; itr = itr->next){
    p = list_entry(itr, struct proc, sibling);
    p->parent = initproc;
    if(p->state == ZOMBIE)
      wakeup1(initproc);
  }
  list_bulk_move_tail(&curproc->children,
                      &initproc->children);

  // Jump into the scheduler, never to return.
  if(curproc->type == MLFQ){
    dequeue(curproc);
  } else { // STRIDE
    ptable.mlfq.tickets += curproc->tickets;
    list_del(&curproc->queue);
  }
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

void
freeproc(struct proc *p)
{
  kfree(p->kstack);
  p->kstack = 0;
  freevm(p->pgdir);
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->killed = 0;
  p->tickets = 0;
  p->pass = 0;
  p->ticks = 0;
  p->privlevel = 0;
  p->state = UNUSED;
  list_add(&p->queue, &ptable.free);
}
// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  struct list_head *children, *itr;
  int pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    children = &curproc->children;
    for(itr = children->next; itr != children; itr = itr->next){
      p = list_entry(itr, struct proc, sibling);
      if(p->state == ZOMBIE){
        pid = p->pid;
        list_del(itr);
        freeproc(p);
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(list_empty(children) || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

struct proc*
mlfqselect(){
  struct proc *p;
  struct list_head *q;
  struct list_head **ppin;
  struct list_head *itr;
  int l;

  for(l = 0; l < QSIZE; l++){
    q = &ptable.mlfq.queue[l];
    ppin = &ptable.mlfq.pin[l];
    itr = *ppin;
    do {
      if(!list_is_head(itr, q)){
        p = list_entry(itr, struct proc, queue);
        if(p->state == RUNNABLE){
          *ppin = itr;
          return p;
        }
      }
      itr = itr->next;
    } while(itr != *ppin);
  }

  return 0;
}

void
mlfqlogic(struct proc *p){
  struct list_head **ppin = &ptable.mlfq.pin[p->privlevel];
  struct list_head *q;
  struct list_head *itr;
  struct proc *pitr;
  int l, baselevel = QSIZE - 1;

  ptable.mlfq.ticks++;
  switch(p->state){
    case RUNNABLE:
      p->ticks++;
      if(p->privlevel < baselevel && p->ticks % TA(p->privlevel) == 0){
        dequeue(p);
        p->privlevel++;
        enqueue(p->privlevel, p);
        p->ticks = 0;
      } else if(p->ticks % TQ(p->privlevel) == 0){
        *ppin = p->queue.next;
      }
      break;
    case SLEEPING:
      if(p->privlevel < baselevel && p->ticks >= TA(p->privlevel)){
        p->privlevel++;
        p->ticks = 0;
      } else {
        p->ticks = p->ticks / TQ(p->privlevel) * TQ(p->privlevel);
      }
      break;
    case ZOMBIE:
      break;
    default:
      panic("mlfq wrong state");
  }
  // Priority boost
  if(ptable.mlfq.ticks % BOOSTINTERVAL == 0){
    // RUNNABLE, RUNNING
    for(l = 1; l <= baselevel; l++){
      q = &ptable.mlfq.queue[l];
      for(itr = q->next; itr != q; itr = itr->next){
        pitr = list_entry(itr, struct proc, queue);
        pitr->privlevel = 0;
        pitr->ticks = 0;
      }
      concatqueue(l, 0);
    }
    // SLEEPING
    q = &ptable.sleep;
    for(itr = q->next; itr != q; itr = itr->next){
      pitr = list_entry(itr, struct proc, queue);
      pitr->privlevel = 0;
      pitr->ticks = 0;
    }
  }
}

void
stridelogic(struct proc *p){
  struct list_head *q;
  struct list_head *itr;
  struct proc *pitr;
  int minpass;
  int i;

  // Pass overflow handling
  minpass = p == 0 || p->type == MLFQ ?
    ptable.mlfq.pass : p->pass;
  if(minpass > BARRIER){
    for(i = 1; i <= ptable.stride.size; i++){
      ptable.stride.minheap[i]->pass -= minpass;
    }
    q = &ptable.stride.run;
    for(itr = q->next; itr != q; itr = itr->next){
      pitr = list_entry(itr, struct proc, queue);
      pitr->pass -= minpass;
    }
    ptable.mlfq.pass -= minpass;
  }

  // Pass increases by stride
  if(p == 0 || p->type == MLFQ){
    ptable.mlfq.pass += STRD(ptable.mlfq.tickets);
  } else if(p->type == STRIDE){
    if(p->state == RUNNABLE || p->state == SLEEPING){
      p->pass += STRD(p->tickets);
      pushheap(p);
    }
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
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    sti();

    acquire(&ptable.lock);

    // Select next process
    p = getminpass() < ptable.mlfq.pass ?
      popheap() : mlfqselect();

    // Run process
    if(p != 0 && p->state == RUNNABLE) {
      if(p->type == STRIDE)
        list_add(&p->queue, &ptable.stride.run);

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      if(p->type == MLFQ){
        mlfqlogic(p);
      }
      c->proc = 0;
    }

    // Log stride
    stridelogic(p);

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
  struct proc *p;

  acquire(&ptable.lock);  //DOC: yieldlock
  p = myproc();
  if(p->type == STRIDE)
    list_del(&p->queue);
  p->state = RUNNABLE;
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
  if(p->type == MLFQ){
    dequeue(p);
  } else {
    list_del(&p->queue);
  }
  p->state = SLEEPING;
  list_add(&p->queue, &ptable.sleep);

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
  struct list_head *q;
  struct list_head *itr, *prev;

  q = &ptable.sleep;
  for(itr = q->next; itr != q; itr = itr->next){
    p = list_entry(itr, struct proc, queue);
    if(p->chan == chan){
      prev = itr->prev;
      list_del(itr);
      p->state = RUNNABLE;
      if(p->type == MLFQ)
        enqueue(p->privlevel, p);
      itr = prev;
    }
  }
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
      if(p->state == SLEEPING){
        list_del(&p->queue);
        p->state = RUNNABLE;
        if(p->type == MLFQ)
          enqueue(p->privlevel, p);
      }
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
    cprintf("%d %d %s %s", p->pid, p->privlevel, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
