/*********************************************************
 * Copyright (C) 2009 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#include "vmware.h"
#include "str.h"
#include "util.h"
#include "userlock.h"
#include "ulInt.h"

struct MXUserRecLock
{
   MXUserHeader            header;
   MXRecLock               recursiveLock;

   uint64                  holdStart;

#if defined(MXUSER_STATS)
   MXUserAcquisitionStats  acquisitionStats;
   Atomic_Ptr              acquisitionHisto;

   MXUserBasicStats        heldStats;
   Atomic_Ptr              heldHisto;
#endif

   /*
    * This is the MX recursive lock override pointer. It is used within the
    * VMX only.
    *
    *  NULL   Use the MXRecLock within this structure
    *         MXUser_CreateRecLock was used to create the lock
    *
    * !NULL   Use the MX_MutexRec pointed to by vmmLock
    *         MXUser_BindMXMutexRec was used to create the lock
    */

   struct MX_MutexRec     *vmmLock;
};

#if defined(MXUSER_STATS)
/*
 *-----------------------------------------------------------------------------
 *
 * MXUserStatsActionRec --
 *
 *      Perform the statistics action for the specified lock.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
MXUserStatsActionRec(MXUserHeader *header,  // IN:
                     unsigned epoch)        // IN:
{
   Bool isHot;
   Bool doLog;
   double contentionRatio;

   MXUserRecLock *lock = (MXUserRecLock *) header;

   /*
    * Dump the statistics for the specified lock.
    */

   MXUserDumpAcquisitionStats(epoch, MXUSER_STAT_CLASS_ACQUISITION, header,
                              &lock->acquisitionStats);

   if (Atomic_ReadPtr(&lock->acquisitionHisto) != NULL) {
      MXUserHistoDump(epoch, MXUSER_STAT_CLASS_ACQUISITION, header,
                      Atomic_ReadPtr(&lock->acquisitionHisto));
   }

   MXUserDumpBasicStats(epoch, MXUSER_STAT_CLASS_HELD, header,
                        &lock->heldStats);

   if (Atomic_ReadPtr(&lock->heldHisto) != NULL) {
      MXUserHistoDump(epoch, MXUSER_STAT_CLASS_HELD, header,
                      Atomic_ReadPtr(&lock->heldHisto));
   }

   /*
    * Has the lock gone "hot"? If so, implement the hot actions.
    */

   MXUserKitchen(&lock->acquisitionStats, &contentionRatio, &isHot, &doLog);

   if (isHot) {
      MXUserForceHisto(&lock->acquisitionHisto,
                       MXUSER_DEFAULT_HISTO_MIN_VALUE_NS,
                       MXUSER_DEFAULT_HISTO_DECADES);
      MXUserForceHisto(&lock->heldHisto,
                       MXUSER_DEFAULT_HISTO_MIN_VALUE_NS,
                       MXUSER_DEFAULT_HISTO_DECADES);

      if (doLog) {
         Log("HOT LOCK (%s); contention ratio %f\n",
             lock->header.lockName, contentionRatio);
      }
   }
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserDumpRecLock --
 *
 *      Dump an recursive lock.
 *
 * Results:
 *      A dump.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
MXUserDumpRecLock(MXUserHeader *header)  // IN:
{
   MXUserRecLock *lock = (MXUserRecLock *) header;

   Warning("%s: Recursive lock @ %p\n", __FUNCTION__, lock);

   Warning("\tsignature %X\n", lock->header.lockSignature);
   Warning("\tname %s\n", lock->header.lockName);
   Warning("\trank 0x%x\n", lock->header.lockRank);

   if (lock->vmmLock == NULL) {
      Warning("\tcount %u\n", lock->recursiveLock.referenceCount);

#if defined(MXUSER_DEBUG)
      Warning("\tcaller %p\n", lock->recursiveLock.ownerRetAddr);
      Warning("\tVThreadID %d\n", (int) lock->recursiveLock.portableThreadID);
#endif
   } else {
      Warning("\tvmmLock %p\n", lock->vmmLock);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_CreateRecLock --
 *
 *      Create a recursive lock.
 *
 *      Only the owner (thread) of a recursive lock may recurse on it.
 *
 * Results:
 *      NULL  Creation failed
 *      !NULL Creation succeeded
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MXUserRecLock *
MXUser_CreateRecLock(const char *userName,  // IN:
                     MX_Rank rank)          // IN:
{
   char *properName;
   MXUserRecLock *lock;

   lock = Util_SafeCalloc(1, sizeof(*lock));

   if (userName == NULL) {
      properName = Str_SafeAsprintf(NULL, "R-%p", GetReturnAddress());
   } else {
      properName = Util_SafeStrdup(userName);
   }

   if (!MXRecLockInit(&lock->recursiveLock)) {
      free(properName);
      free(lock);

      return NULL;
   }

   lock->vmmLock = NULL;

   lock->header.lockName = properName;
   lock->header.lockSignature = USERLOCK_SIGNATURE;
   lock->header.lockRank = rank;
   lock->header.lockDumper = MXUserDumpRecLock;

#if defined(MXUSER_STATS)
   lock->header.lockStatsAction = MXUserStatsActionRec;
   lock->header.lockID = MXUserAllocID();

   MXUserAddToList(&lock->header);
   MXUserAcquisitionStatsSetUp(&lock->acquisitionStats);
   MXUserBasicStatsSetUp(&lock->heldStats);
#endif

   return lock;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_DestroyRecLock --
 *
 *      Destroy a recursive lock.
 *
 *      When the lock is bound to a MX lock, only the MXUser "wrapper" is
 *      freed. The caller is responsible for calling MX_DestroyLockRec() on
 *      the MX lock.
 *
 * Results:
 *      Lock is destroyed. Don't use the pointer again.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_DestroyRecLock(MXUserRecLock *lock)  // IN:
{
   if (lock != NULL) {
      ASSERT(lock->header.lockSignature == USERLOCK_SIGNATURE);

      if (lock->vmmLock == NULL) {
         if (MXRecLockCount(&lock->recursiveLock) > 0) {
            MXUserDumpAndPanic(&lock->header,
                               "%s: Destroy of an acquired recursive lock\n",
                               __FUNCTION__);
         }

         MXRecLockDestroy(&lock->recursiveLock);

#if defined(MXUSER_STATS)
         MXUserRemoveFromList(&lock->header);
         MXUserHistoTearDown(Atomic_ReadPtr(&lock->acquisitionHisto));
         MXUserHistoTearDown(Atomic_ReadPtr(&lock->heldHisto));
#endif
      }

      lock->header.lockSignature = 0;  // just in case...
      free((void *) lock->header.lockName);  // avoid const warnings
      lock->header.lockName = NULL;
      free(lock);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_AcquireRecLock --
 *
 *      An acquisition is made (lock is taken) on the specified recursive lock.
 *
 *      Only the owner (thread) of a recursive lock may recurse on it.
 *
 * Results:
 *      The lock is acquired (locked).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_AcquireRecLock(MXUserRecLock *lock)  // IN/OUT:
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   if (lock->vmmLock) {
      ASSERT(MXUserMX_LockRec);
      (*MXUserMX_LockRec)(lock->vmmLock);
   } else {
#if defined(MXUSER_STATS)
      Bool contended;
      uint64 begin;
#endif
      /* Rank checking is only done on the first acquisition */
      MXUserAcquisitionTracking(&lock->header, TRUE);

#if defined(MXUSER_STATS)
      begin = MXUserReadTimerNS();

      contended =
#endif

      MXRecLockAcquire(&lock->recursiveLock, GetReturnAddress());

#if defined(MXUSER_STATS)
      if (MXRecLockCount(&lock->recursiveLock) == 1) {
         MXUserHisto *histo;
         uint64 value = MXUserReadTimerNS() - begin;

         MXUserAcquisitionSample(&lock->acquisitionStats, contended, value);

         histo = Atomic_ReadPtr(&lock->acquisitionHisto);

         if (UNLIKELY(histo != NULL)) {
            MXUserHistoSample(histo, value);
         }

         lock->holdStart = MXUserReadTimerNS();
      }
#endif
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_ReleaseRecLock --
 *
 *      Release (unlock) a recursive lock.
 *
 * Results:
 *      The lock is released.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_ReleaseRecLock(MXUserRecLock *lock)  // IN/OUT:
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   if (lock->vmmLock) {
      ASSERT(MXUserMX_UnlockRec);
      (*MXUserMX_UnlockRec)(lock->vmmLock);
   } else {
#if defined(MXUSER_STATS)
      if (MXRecLockCount(&lock->recursiveLock) == 1) {
         uint64 value = MXUserReadTimerNS() - lock->holdStart;
         MXUserHisto *histo = Atomic_ReadPtr(&lock->heldHisto);

         MXUserBasicStatsSample(&lock->heldStats, value);

         if (UNLIKELY(histo != NULL)) {
            MXUserHistoSample(histo, value);
         }
      }
#endif

      if (!MXRecLockIsOwner(&lock->recursiveLock)) {
         uint32 lockCount = MXRecLockCount(&lock->recursiveLock);

         MXUserDumpAndPanic(&lock->header,
                            "%s: Non-owner release of an %s recursive lock\n",
                            __FUNCTION__,
                            lockCount == 0 ? "unacquired" : "acquired");
      }

      MXUserReleaseTracking(&lock->header);

      MXRecLockRelease(&lock->recursiveLock);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_TryAcquireRecLock --
 *
 *      An attempt is made to conditionally acquire (lock) a recursive lock.
 *
 *      Only the owner (thread) of a recursive lock may recurse on it.
 *
 * Results:
 *      TRUE    Acquired (locked)
 *      FALSE   Not acquired
 *
 * Side effects:
 *      None
 *
 * NOTE:
 *      A "TryAcquire" does not rank check should the acquisition succeed.
 *      This duplicates the behavor of MX locks.
 *
 *-----------------------------------------------------------------------------
 */

Bool
MXUser_TryAcquireRecLock(MXUserRecLock *lock)  // IN/OUT:
{
   Bool success;

   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   if (lock->vmmLock) {
      ASSERT(MXUserMX_TryLockRec);
      success = (*MXUserMX_TryLockRec)(lock->vmmLock);
   } else {
#if defined(MXUSER_STATS)
      uint64 begin;
#endif

      if (MXUserTryAcquireFail(lock->header.lockName)) {
         return FALSE;
      }

#if defined(MXUSER_STATS)
      begin = MXUserReadTimerNS();
#endif

      success = MXRecLockTryAcquire(&lock->recursiveLock, GetReturnAddress());

      if (success) {
#if defined(MXUSER_STATS)
         if (MXRecLockCount(&lock->recursiveLock) == 1) {
            MXUserAcquisitionSample(&lock->acquisitionStats, FALSE,
                                  MXUserReadTimerNS() - begin);
         }
#endif

         MXUserAcquisitionTracking(&lock->header, FALSE);
      }
   }

   return success;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_IsCurThreadHoldingRecLock --
 *
 *      Is a recursive lock held by the calling thread?
 *
 * Results:
 *      TRUE    Yes
 *      FALSE   No
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
MXUser_IsCurThreadHoldingRecLock(const MXUserRecLock *lock)  // IN:
{
   Bool result;
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   if (lock->vmmLock) {
      ASSERT(MXUserMX_IsLockedByCurThreadRec);
      result = (*MXUserMX_IsLockedByCurThreadRec)(lock->vmmLock);
   } else {
      result = MXRecLockIsOwner(&lock->recursiveLock);
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_ControlRecLock --
 *
 *      Perform the specified command on the specified lock.
 *
 * Results:
 *      TRUE    succeeded
 *      FALSE   failed
 *
 * Side effects:
 *      Depends on the command, no?
 *
 *-----------------------------------------------------------------------------
 */

Bool
MXUser_ControlRecLock(MXUserRecLock *lock,  // IN/OUT:
                      uint32 command,       // IN:
                      ...)                  // IN:
{
   Bool result;

   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));
   ASSERT(lock->vmmLock == NULL);  // only unbound locks

   switch (command) {
#if defined(MXUSER_STATS)
   case MXUSER_CONTROL_ACQUISITION_HISTO: {
      va_list a;
      uint64 minValue;
      uint32 decades;

      va_start(a, command);
      minValue = va_arg(a, uint64);
      decades = va_arg(a, uint32);
      va_end(a);

      MXUserForceHisto(&lock->acquisitionHisto, minValue, decades);

      result = TRUE;
      break;
   }

   case MXUSER_CONTROL_HELD_HISTO: {
      va_list a;
      uint64 minValue;
      uint32 decades;

      va_start(a, command);
      minValue = va_arg(a, uint64);
      decades = va_arg(a, uint32);
      va_end(a);

      MXUserForceHisto(&lock->heldHisto, minValue, decades);
      
      result = TRUE;
      break; 
   }
#endif

   default:
      result = FALSE;
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_CreateSingletonRecLock --
 *
 *      Ensures that the specified backing object (Atomic_Ptr) contains a
 *      recursive lock. This is useful for modules that need to protect
 *      something with a lock but don't have an existing Init() entry point
 *      where a lock can be created.
 *
 * Results:
 *      A pointer to the requested lock.
 *
 * Side effects:
 *      Generally the lock's resources are intentionally leaked (by design).
 *
 *-----------------------------------------------------------------------------
 */

MXUserRecLock *
MXUser_CreateSingletonRecLock(Atomic_Ptr *lockStorage,  // IN/OUT:
                              const char *name,         // IN:
                              MX_Rank rank)             // IN:
{
   MXUserRecLock *lock;

   ASSERT(lockStorage);

   lock = (MXUserRecLock *) Atomic_ReadPtr(lockStorage);

   if (UNLIKELY(lock == NULL)) {
      MXUserRecLock *before;

      lock = MXUser_CreateRecLock(name, rank);

      before = (MXUserRecLock *) Atomic_ReadIfEqualWritePtr(lockStorage, NULL,
                                                            (void *) lock);

      if (before) {
         MXUser_DestroyRecLock(lock);

         lock = before;
      }
   }

   return lock;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_CreateCondVarRecLock --
 *
 *      Create a condition variable for use with the specified recurisve lock.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      The created condition variable will cause a run-time error if it is
 *      used with a lock other than the one it was created for.
 *
 *-----------------------------------------------------------------------------
 */

MXUserCondVar *
MXUser_CreateCondVarRecLock(MXUserRecLock *lock)
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));
   ASSERT(lock->vmmLock == NULL);  // only unbound locks

   return MXUserCreateCondVar(&lock->header, &lock->recursiveLock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_WaitCondVarRecLock --
 *
 *      Block (sleep) on the specified condition variable. The specified lock
 *      is released upon blocking and is reacquired before returning from this
 *      function.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_WaitCondVarRecLock(MXUserRecLock *lock,     // IN:
                          MXUserCondVar *condVar)  // IN:
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));
   ASSERT(lock->vmmLock == NULL);  // only unbound locks

   MXUserWaitCondVar(&lock->header, &lock->recursiveLock, condVar);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_GetRecLockVmm --
 *
 *      Return lock->vmmLock. Perhaps this lock is bound to an MX lock.
 *
 * Results:
 *      lock->vmmLock is returned.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

struct MX_MutexRec *
MXUser_GetRecLockVmm(const MXUserRecLock *lock)  // IN:
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   return lock->vmmLock;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_GetRecLockRank --
 *
 *      Return the rank of the specified recursive lock.
 *
 * Results:
 *      The rank of the specified recursive lock is returned.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MX_Rank
MXUser_GetRecLockRank(const MXUserRecLock *lock)  // IN:
{
   ASSERT(lock && (lock->header.lockSignature == USERLOCK_SIGNATURE));

   return lock->header.lockRank;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_BindMXMutexRec --
 *
 *      Create an MXUserRecLock that is bound to an (already) initialized
 *      MX_MutexRec.
 *
 * Results:
 *      NULL  Creation failed
 *      !NULL Creation succeeded
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MXUserRecLock *
MXUser_BindMXMutexRec(struct MX_MutexRec *mutex,  // IN:
                      MX_Rank rank)               // IN:
{
   MXUserRecLock *lock;

   ASSERT(mutex);

   /*
    * Cannot perform a binding unless MX_Init has been called. As a side
    * effect it registers these hook functions.
    */

   if ((MXUserMX_LockRec == NULL) ||
       (MXUserMX_UnlockRec == NULL) ||
       (MXUserMX_TryLockRec == NULL) ||
       (MXUserMX_IsLockedByCurThreadRec == NULL)) {
       return NULL;
    }

   /*
    * Initialize the header (so it looks correct in memory) but don't connect
    * this lock to the MXUser statistics or debugging tracking - the MX lock
    * system will take care of this.
    */

   lock = Util_SafeCalloc(1, sizeof(*lock));

   lock->header.lockName = Str_SafeAsprintf(NULL, "MX_%p", mutex);

   lock->header.lockSignature = USERLOCK_SIGNATURE;
   lock->header.lockRank = rank;
   lock->header.lockDumper = NULL;

#if defined(MXUSER_STATS)
   lock->header.lockStatsAction = NULL;
   lock->header.lockID = MXUserAllocID();
#endif

   lock->vmmLock = mutex;

   return lock;
}


#if defined(VMX86_VMX)
#include "mutex.h"
#include "mutexRank.h"

/*
 *----------------------------------------------------------------------------
 *
 * MXUser_InitFromMXRec --
 *
 *      Initialize a MX_MutexRec lock and create a MXUserRecLock that binds
 *      to it.
 *
 * Results:
 *      Pointer to the MXUserRecLock.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

MXUserRecLock *
MXUser_InitFromMXRec(const char *name,    // IN:
                     MX_MutexRec *mutex,  // IN:
                     MX_Rank rank,        // IN:
                     Bool isBelowBull)    // IN:
{
   MXUserRecLock *userLock;

   ASSERT(isBelowBull == (rank < RANK_userlevelLock));

   MX_InitLockRec(name, rank, mutex);
   userLock = MXUser_BindMXMutexRec(mutex, rank);
   ASSERT(userLock);

   return userLock;
}
#endif