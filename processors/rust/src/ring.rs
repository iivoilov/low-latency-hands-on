//! Bounded whole-slot SPSC ownership, with no overwriting or allocation in use.
use std::{
    cell::{Cell, UnsafeCell},
    marker::PhantomData,
    ops::{Deref, DerefMut},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
};

#[repr(align(64))]
struct Index(AtomicUsize);
struct Ring<T> {
    slots: Box<[UnsafeCell<T>]>,
    write: Index,
    read: Index,
}
// SAFETY: The only access is through the unique producer/consumer endpoints.
// A release/acquire handoff transfers each slot; guards keep it owned until
// publication/release. Guards are !Sync even when T is Send but not Sync.
unsafe impl<T: Send> Sync for Ring<T> {}
pub struct Producer<T> {
    ring: Arc<Ring<T>>,
}
pub struct Consumer<T> {
    ring: Arc<Ring<T>>,
}
pub struct WriteGuard<'a, T> {
    ring: &'a Ring<T>,
    index: usize,
    marker: PhantomData<Cell<()>>,
}
pub struct ReadGuard<'a, T> {
    ring: &'a Ring<T>,
    index: usize,
    marker: PhantomData<Cell<()>>,
}

pub fn channel<T: Default + Send>(capacity: usize) -> Result<(Producer<T>, Consumer<T>), String> {
    if !capacity.is_power_of_two() {
        return Err("Queue capacity must be a power of two".into());
    }
    let ring = Arc::new(Ring {
        slots: (0..capacity)
            .map(|_| UnsafeCell::new(T::default()))
            .collect::<Vec<_>>()
            .into_boxed_slice(),
        write: Index(AtomicUsize::new(0)),
        read: Index(AtomicUsize::new(0)),
    });
    Ok((Producer { ring: ring.clone() }, Consumer { ring }))
}
impl<T> Producer<T> {
    pub fn reserve(&mut self) -> Option<WriteGuard<'_, T>> {
        let index = self.ring.write.0.load(Ordering::Relaxed);
        if index.wrapping_sub(self.ring.read.0.load(Ordering::Acquire)) == self.ring.slots.len() {
            return None;
        }
        Some(WriteGuard {
            ring: &self.ring,
            index,
            marker: PhantomData,
        })
    }
    pub fn try_push(&mut self, value: T) -> bool {
        if let Some(mut slot) = self.reserve() {
            *slot = value;
            slot.commit();
            true
        } else {
            false
        }
    }
}
impl<T> Consumer<T> {
    pub fn read(&mut self) -> Option<ReadGuard<'_, T>> {
        let index = self.ring.read.0.load(Ordering::Relaxed);
        if index == self.ring.write.0.load(Ordering::Acquire) {
            return None;
        }
        Some(ReadGuard {
            ring: &self.ring,
            index,
            marker: PhantomData,
        })
    }
    pub fn is_empty(&self) -> bool {
        self.ring.read.0.load(Ordering::Relaxed) == self.ring.write.0.load(Ordering::Acquire)
    }
    pub fn try_pop(&mut self) -> Option<T>
    where
        T: Copy,
    {
        self.read().map(|slot| *slot)
    }
}
impl<T> Deref for WriteGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: This guard exclusively owns an unpublished slot.
        unsafe { &*self.ring.slots[self.index & (self.ring.slots.len() - 1)].get() }
    }
}
impl<T> DerefMut for WriteGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: No reader can access this slot until commit consumes the guard.
        unsafe { &mut *self.ring.slots[self.index & (self.ring.slots.len() - 1)].get() }
    }
}
impl<T> WriteGuard<'_, T> {
    pub fn commit(self) -> usize {
        let next = self.index.wrapping_add(1);
        let depth = next.wrapping_sub(self.ring.read.0.load(Ordering::Acquire));
        self.ring.write.0.store(next, Ordering::Release);
        depth
    }
}
impl<T> Deref for ReadGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: The acquire above observed publication. Drop releases ownership.
        unsafe { &*self.ring.slots[self.index & (self.ring.slots.len() - 1)].get() }
    }
}
impl<T> Drop for ReadGuard<'_, T> {
    fn drop(&mut self) {
        self.ring
            .read
            .0
            .store(self.index.wrapping_add(1), Ordering::Release);
    }
}
