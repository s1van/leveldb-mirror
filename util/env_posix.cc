// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <deque>
#include <set>
#include <string>
#include <iostream>
#include <cstring>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(LEVELDB_PLATFORM_ANDROID)
#include <sys/stat.h>
#endif
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/posix_logger.h"
#include "leveldb/mirror.h"
#include "util/aio_wrapper.h"

namespace leveldb {
pthread_t *mirror_helper = NULL;//for compaction
opq mio_queue = NULL;		//for helper

namespace {

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}

class PosixSequentialFile: public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixSequentialFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) {
    DEBUG_INFO(fname);
  }
  virtual ~PosixSequentialFile() { fclose(file_); }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    Status s;
    size_t r = fread_unlocked(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (feof(file_)) {
        // We leave status as ok if we hit the end of the file
      } else {
        // A partial read with an error: return a non-ok status
        s = IOError(filename_, errno);
      }
    }
    return s;
  }

  virtual Status Skip(uint64_t n) {
    if (fseek(file_, n, SEEK_CUR)) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }
};

// pread() based random-access
class PosixRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  int fd_;

 public:
  PosixRandomAccessFile(const std::string& fname, int fd)
      : filename_(fname), fd_(fd) {
    DEBUG_INFO(fname);
  }
  virtual ~PosixRandomAccessFile() { close(fd_); }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : r);
    if (r < 0) {
      // An error: return a non-ok status
      s = IOError(filename_, errno);
    }
    return s;
  }
};

// Helper class to limit mmap file usage so that we do not end up
// running out virtual memory or running into kernel performance
// problems for very large databases.
class MmapLimiter {
 public:
  // Up to 1000 mmaps for 64-bit binaries; none for smaller pointer sizes.
  MmapLimiter() {
    SetAllowed(sizeof(void*) >= 8 ? 1000 : 0);
  }

  // If another mmap slot is available, acquire it and return true.
  // Else return false.
  bool Acquire() {
    if (GetAllowed() <= 0) {
      return false;
    }
    MutexLock l(&mu_);
    intptr_t x = GetAllowed();
    if (x <= 0) {
      return false;
    } else {
      SetAllowed(x - 1);
      return true;
    }
  }

  // Release a slot acquired by a previous call to Acquire() that returned true.
  void Release() {
    MutexLock l(&mu_);
    SetAllowed(GetAllowed() + 1);
  }

 private:
  port::Mutex mu_;
  port::AtomicPointer allowed_;

  intptr_t GetAllowed() const {
    return reinterpret_cast<intptr_t>(allowed_.Acquire_Load());
  }

  // REQUIRES: mu_ must be held
  void SetAllowed(intptr_t v) {
    allowed_.Release_Store(reinterpret_cast<void*>(v));
  }

  MmapLimiter(const MmapLimiter&);
  void operator=(const MmapLimiter&);
};

// mmap() based random-access
class PosixMmapReadableFile: public RandomAccessFile {
 private:
  std::string filename_;
  void* mmapped_region_;
  size_t length_;
  MmapLimiter* limiter_;
  bool prefetch_;
	AIOWrapper* awp_;

 public:
  // base[0,length-1] contains the mmapped contents of the file.
  PosixMmapReadableFile(const std::string& fname, void* base, size_t length,
                        MmapLimiter* limiter, bool prefetch = false, AIOWrapper* awp = NULL)
      : filename_(fname), mmapped_region_(base), length_(length),
        limiter_(limiter), prefetch_(prefetch), awp_(awp) {
    DEBUG_INFO(fname);
  }

  virtual ~PosixMmapReadableFile() {
	if (prefetch_) {
		free(mmapped_region_);
	} else {
   		munmap(mmapped_region_, length_);
	}
	if (awp_) delete awp_;
	limiter_->Release();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    DEBUG_INFO3(filename_, offset, n);
		if (prefetch_ && awp_ != NULL && !(awp_->isFinished()) )
			awp_->wait();

    Status s;
    if (offset + n > length_) {
      *result = Slice();
      s = IOError(filename_, EINVAL);
    } else {
      *result = Slice(reinterpret_cast<char*>(mmapped_region_) + offset, n);
    }
    DEBUG_INFO3(filename_, offset, n);
    return s;
  }

};

// We preallocate up to an extra megabyte and use memcpy to append new
// data to the file.  This is safe since we either properly close the
// file before reading from it, or for log files, the reading code
// knows enough to skip zero suffixes.
class PosixMmapFile_ : public WritableFile {
 private:
  std::string filename_;
  int fd_;
  size_t page_size_;
  size_t map_size_;       // How much extra memory to map at a time
  char* base_;            // The mapped region
  char* limit_;           // Limit of the mapped region
  char* dst_;             // Where to write next  (in range [base_,limit_])
  char* last_sync_;       // Where have we synced up to
  uint64_t file_offset_;  // Offset of base_ in file

  // Have we done an munmap of unsynced data?
  bool pending_sync_;

  // Roundup x to a multiple of y
  static size_t Roundup(size_t x, size_t y) {
    return ((x + y - 1) / y) * y;
  }

  size_t TruncateToPageBoundary(size_t s) {
    s -= (s & (page_size_ - 1));
    assert((s % page_size_) == 0);
    return s;
  }


 public:
  PosixMmapFile_(const std::string& fname, int fd, size_t page_size)
      : filename_(fname),
        fd_(fd),
        page_size_(page_size),
        map_size_(Roundup(65536, page_size)),
        base_(NULL),
        limit_(NULL),
        dst_(NULL),
        last_sync_(NULL),
        file_offset_(0),
        pending_sync_(false) {
    assert((page_size & (page_size - 1)) == 0);
    DEBUG_INFO2(fname, fd);
  }


  ~PosixMmapFile_() {
    if (fd_ >= 0) {
      PosixMmapFile_::Close();
    }
  }

  bool UnmapCurrentRegion() {
    //DEBUG_INFO(filename_);
      bool result = true;
      if (base_ != NULL) {
        if (last_sync_ < limit_) {
          // Defer syncing this data until next Sync() call, if any
          pending_sync_ = true;
        }
        if (munmap(base_, limit_ - base_) != 0) {
          result = false;
        }
        file_offset_ += limit_ - base_;
        base_ = NULL;
        limit_ = NULL;
        last_sync_ = NULL;
        dst_ = NULL;

        // Increase the amount we map the next time, but capped at 1MB
        if (map_size_ < (1<<20)) {
          map_size_ *= 2;
        }
      }
      return result;
    }

    bool MapNewRegion() {
    //DEBUG_INFO(filename_);
      assert(base_ == NULL);
      if (ftruncate(fd_, file_offset_ + map_size_) < 0) {
        return false;
      }
      void* ptr = mmap(NULL, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd_, file_offset_);
      if (ptr == MAP_FAILED) {
        return false;
      }
      base_ = reinterpret_cast<char*>(ptr);
      limit_ = base_ + map_size_;
      dst_ = base_;
      last_sync_ = base_;
      return true;
    }


  virtual Status Append(const Slice& data) {
    //if(EXCLUDE_FILE(filename_, ".log")) DEBUG_INFO2(filename_, data.size());
    
    const char* src = data.data();
    size_t left = data.size();
    while (left > 0) {
      assert(base_ <= dst_);
      assert(dst_ <= limit_);
      size_t avail = limit_ - dst_;
      if (avail == 0) {
        if (!UnmapCurrentRegion() ||
            !MapNewRegion()) {
          return IOError(filename_, errno);
        }
      }

      size_t n = (left <= avail) ? left : avail;
      memcpy(dst_, src, n);
      dst_ += n;
      src += n;
      left -= n;
    }

    //if(EXCLUDE_FILE(filename_, ".log")) DEBUG_INFO2(filename_, data.size());
    return Status::OK();
  }

  virtual Status Close() {
    Status s;
    size_t unused = limit_ - dst_;
    if (!UnmapCurrentRegion()) {
      s = IOError(filename_, errno);
    } else if (unused > 0) {
      // Trim the extra space at the end of the file
      if (ftruncate(fd_, file_offset_ - unused) < 0) {
        s = IOError(filename_, errno);
      }
    }

    if (close(fd_) < 0) {
      if (s.ok()) {
        s = IOError(filename_, errno);
      }
    }

    fd_ = -1;
    base_ = NULL;
    limit_ = NULL;
    return s;
  }

  virtual Status Flush() {
    return Status::OK();
  }

  virtual Status Sync(int flags) {
    Status s;

    if (pending_sync_) {
      // Some unmapped data was not synced
      pending_sync_ = false;
      if (fdatasync(fd_) < 0) {
        s = IOError(filename_, errno);
      }
    }

    if (dst_ > last_sync_) {
      // Find the beginnings of the pages that contain the first and last
      // bytes to be synced.
      size_t p1 = TruncateToPageBoundary(last_sync_ - base_);
      size_t p2 = TruncateToPageBoundary(dst_ - base_ - 1);
      last_sync_ = dst_;
			if (flags == 0) flags = MS_SYNC;
      if (msync(base_ + p1, p2 - p1 + page_size_, flags) < 0) {
        s = IOError(filename_, errno);
      }
    }

    return s;
  }
};

static void * mirrorCompactionHelper(void * arg) {
	opq mio_queue = (opq) arg;
	mio_op op;
	PosixMmapFile_ *mfp;
	struct timespec wtime = {0, 16000000}; //ToDo: interval
	int c = 0;

	DEBUG_INFO2("Run_Helper", mio_queue);
	while(1) {
		if (OPQ_NONEMPTY(mio_queue)) {

			OPQ_POP(mio_queue, op);			//operation
			//DEBUG_INFO3("OPQ_POP", op->type, op);

			if (op->type == MSync) {
				mfp = (PosixMmapFile_*) op->ptr1;	//file handler
				//Status s = mfp->Sync(MS_ASYNC);
				//DEBUG_INFO3("MSync[E]", mfp, s.ToString());

			} else if (op->type == MAppend) {
				mfp = (PosixMmapFile_*) op->ptr1;	//file handler
				//Status s = mfp->Append(*((const Slice *) op->ptr2));
				free((void*) (((const Slice *) op->ptr2)->data() ));	//it is malloc-ed
				delete ((Slice *) op->ptr2);
				//DEBUG_INFO3("MAppend[E]", ((const Slice *) op->ptr2)->size(), s.ToString());

			} else if (op->type == MClose) {
				mfp = (PosixMmapFile_*) op->ptr1;	//file handler
				Status s = mfp->Close();
				//DEBUG_INFO3("MClose[E]", op, s.ToString());

			} else if (op->type == MDelete) {
				std::string *fname = (std::string*) (op->ptr1);
				int ret = unlink(fname->c_str());
				DEBUG_INFO2("MDelete[E]", fname);
				delete fname;

			} else if (op->type == MHalt) {
				//DEBUG_INFO("MHalt");
				break;
			}

			free(op);
			continue;
		}
		nanosleep(&wtime, NULL);
		DEBUG_INFO2("Helper_count", c++);
	}

  return NULL;
}

class PosixMmapFile : public WritableFile {
 private:
  std::string filename_;
  std::string mfilename_;
  int fd_;
  int mfd_;
  size_t page_size_;
  PosixMmapFile_ *fp_;
  PosixMmapFile_ *mfp_;


  bool UnmapCurrentRegion() {
    DEBUG_INFO2(filename_, mfilename_);
    return fp_->UnmapCurrentRegion(); //&& mfp_->UnmapCurrentRegion(); //ToDo
  }

  bool MapNewRegion() {
    DEBUG_INFO2(filename_, mfilename_);
    return fp_->MapNewRegion() && mfp_->MapNewRegion();
  }

 public:
  PosixMmapFile(const std::string& fname, int fd, size_t page_size, int mfd)
      : filename_(fname),
        fd_(fd),
        mfd_(mfd),
        page_size_(page_size) {
		assert((page_size & (page_size - 1)) == 0);

    if (MIRROR_ENABLE) {
			mfilename_ = std::string(MIRROR_PATH) + fname.substr(fname.find_last_of("/"));
    	mfp_ = new PosixMmapFile_(mfilename_, mfd, page_size);
    }
    fp_ = new PosixMmapFile_(fname, fd, page_size);
    DEBUG_INFO(filename_);

#ifdef USE_OPQ_THREAD
		INIT_HELPER_AND_QUEUE(mirror_helper, mio_queue);
#endif
  }


  ~PosixMmapFile() {
    if (fd_ >= 0) {
      PosixMmapFile::Close();
    }
  }

  virtual Status Append(const Slice& data) {
		Slice *mdata = data.clone();
    //DEBUG_INFO3(filename_, data.compare((*mdata)), data.size());
#ifdef USE_OPQ_THREAD
    OPQ_ADD_APPEND(mio_queue, mfp_, mdata);
#else
    Status ms = mfp_->Append(data);
    if (!ms.ok())
      return ms;
#endif
    Status s = fp_->Append(data);
    //DEBUG_INFO3("OAppend[E]", data.compare((*mdata)), mdata->size());
    return s;
  }

  virtual Status Close() {
#ifdef USE_OPQ_THREAD
    OPQ_ADD_CLOSE(mio_queue, mfp_);
#else
    Status ms = mfp_->Close();
    if (!ms.ok())
      return ms;
#endif
    Status s = fp_->Close();
		DEBUG_INFO3("OClose", mfp_, s.ToString());
		fd_ = -1;
    return s;
  }

  virtual Status Flush() {
    return Status::OK();
  }

  virtual Status Sync(int flags) {
    DEBUG_INFO3("Sync Starts", filename_, mfilename_);
#ifdef USE_OPQ_THREAD
    OPQ_ADD_SYNC(mio_queue, mfp_);
#else
    //Status ms = mfp_->Sync(MS_ASYNC);
    //if (!ms.ok())
    //return ms;
#endif
    Status s = fp_->Sync(MS_SYNC);

    DEBUG_INFO3("Sync Ends", filename_, mfilename_);
    return s;
	//return Status::OK();
  }
};

static int LockOrUnlock(int fd, bool lock) {
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;        // Lock/unlock entire file
  return fcntl(fd, F_SETLK, &f);
}

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string name_;
};

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
class PosixLockTable {
 private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;
 public:
  bool Insert(const std::string& fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

class PosixEnv : public Env {
 public:
  PosixEnv();
  virtual ~PosixEnv() {
    fprintf(stderr, "Destroying Env::Default()\n");
    abort();
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    FILE* f = fopen(fname.c_str(), "r");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixSequentialFile(fname, f);
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result, bool mirror) {
    DEBUG_INFO(fname);
    *result = NULL;
    Status s;
		
		int flags = O_RDONLY;
		//if (MIRROR_ENABLE && mirror)
			//flags = flags | O_DIRECT; 

    int fd = open(fname.c_str(), flags);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else if (mmap_limit_.Acquire()) {
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
				if (MIRROR_ENABLE && mirror) {
					void* base = (void*) malloc(size);
					ssize_t ret = pread(fd, base, size, 0);
					//AIOWrapper *awp = new AIOWrapper();
					//ssize_t ret = awp->read(fd, base, size, 0);
					*result = new PosixMmapReadableFile(fname, base, size, &mmap_limit_, true, NULL);
				} else {
        	void* base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        	if (base != MAP_FAILED) {
          		*result = new PosixMmapReadableFile(fname, base, size, &mmap_limit_);
        	} else {
          		s = IOError(fname, errno);
        	}
	}
      }
	if (!(MIRROR_ENABLE && mirror))	// to perform AIO, fd is closed later
      close(fd);
      if (!s.ok()) {
        mmap_limit_.Release();
      }
    } else {
      *result = new PosixRandomAccessFile(fname, fd);
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    Status s;
    std::string mfname;
    bool mirror = MIRROR_ENABLE ? EXCLUDE_FILES(fname) : false;

    const int fd = open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0777);
    int mfd;
    if (mirror) {
      mfname = std::string(MIRROR_PATH) + fname.substr(fname.find_last_of("/"));
      mfd = open(mfname.c_str(), O_CREAT | O_RDWR | O_TRUNC , 0777);
			DEBUG_INFO3(fname, mfname, mirror);
    } else {
      mfd = 1;
			DEBUG_INFO2(fname, mirror);
		}

    if (fd < 0) {
      *result = NULL;
      s = IOError(fname, errno);
    } else if (mfd < 0){
      *result = NULL;
      s = IOError(mfname, errno);
    } else {
      if (mirror) {
        *result = new PosixMmapFile(fname, fd, page_size_, mfd);
      } else {
        *result = new PosixMmapFile_(fname, fd, page_size_);
      }
    }

    return s;
  }

  virtual bool FileExists(const std::string& fname) {
    return access(fname.c_str(), F_OK) == 0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
      return IOError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    
    if (unlink(fname.c_str()) != 0) {
      result = IOError(fname, errno);
    }

    bool mirror = MIRROR_ENABLE ? EXCLUDE_FILES(fname) : false;
		if (mirror) {
	    std::string	mfname = std::string(MIRROR_PATH) + fname.substr(fname.find_last_of("/"));
#ifdef USE_OPQ_THREAD
			INIT_HELPER_AND_QUEUE(mirror_helper, mio_queue);
			OPQ_ADD_DELETE(mio_queue, new std::string(mfname) );
#else
			if (unlink(mfname.c_str()) != 0) {
				result = IOError(fname, errno);
			}
#endif
		}
    DEBUG_INFO(fname);
    return result;
  }

  virtual Status CreateDir(const std::string& name) {
    Status result;
    DEBUG_INFO(name);
    if (mkdir(name.c_str(), 0755) != 0) {
      result = IOError(name, errno);
    }
    return result;
  }

  virtual Status DeleteDir(const std::string& name) {
    Status result;
    DEBUG_INFO(name);
    if (rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    DEBUG_INFO(fname);
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

	//ToDo: Add to OPQ
  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;
    bool mirror = MIRROR_ENABLE ? EXCLUDE_FILES(src) : false;

    DEBUG_INFO2(src + "\t" + target, mirror);

    if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    if (mirror) {
    	const std::string msrc = std::string(MIRROR_PATH) + src.substr(src.find_last_of("/"));
    	const std::string mtarget = std::string(MIRROR_PATH) + target.substr(target.find_last_of("/"));
			if (rename(msrc.c_str(), mtarget.c_str()) != 0) {
				result = IOError(msrc, errno);
			}
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = NULL;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock(fd, true) == -1) {
      result = IOError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->fd_, false) == -1) {
      result = IOError("unlock", errno);
    }
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
    DEBUG_INFO(fname);
    FILE* f = fopen(fname.c_str(), "w");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixLogger(f, &PosixEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

 private:
  void PthreadCall(const char* label, int result) {
    if (result != 0) {
      fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
      abort();
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PosixEnv*>(arg)->BGThread();
    return NULL;
  }

  size_t page_size_;
  pthread_mutex_t mu_;
  pthread_cond_t bgsignal_;
  pthread_t bgthread_;
  bool started_bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  PosixLockTable locks_;
  MmapLimiter mmap_limit_;
};

PosixEnv::PosixEnv() : page_size_(getpagesize()),
                       started_bgthread_(false) {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, NULL));
  PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, NULL));
}

void PosixEnv::Schedule(void (*function)(void*), void* arg) {
  PthreadCall("lock", pthread_mutex_lock(&mu_));

  // Start background thread if necessary
  if (!started_bgthread_) {
    started_bgthread_ = true;
    PthreadCall(
        "create thread",
        pthread_create(&bgthread_, NULL,  &PosixEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  if (queue_.empty()) {
    PthreadCall("signal", pthread_cond_signal(&bgsignal_));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PosixEnv::BGThread() {
  while (true) {
    // Wait until there is an item that is ready to run
    PthreadCall("lock", pthread_mutex_lock(&mu_));
    while (queue_.empty()) {
      PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
    }

    void (*function)(void*) = queue_.front().function;
    void* arg = queue_.front().arg;
    queue_.pop_front();

    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    (*function)(arg);
  }
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}
static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return NULL;
}

void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
		pthread_create(&t, NULL,  &StartThreadWrapper, state));
}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PosixEnv; }

Env* Env::Default() {
  pthread_once(&once, InitDefaultEnv);
  return default_env;
}

}  // namespace leveldb
