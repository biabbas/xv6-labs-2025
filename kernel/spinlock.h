// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  uint32 readers;      // Number of readers holding the lock.
  uint32 writers;       // Is there a writer holding the lock?
  struct spinlock writer_lock; // Lock to protect writer access.
};
#endif
