#ifndef MIRROR_LEVELDB_H
#define MIRROR_LEVELDB_H

#include "leveldb/debug.h"
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>
#include "util/hash.h"

extern int MIRROR_ENABLE;
extern const char *MIRROR_PATH;

#define BLKSIZE 4096

/************************** Configuration Macros *****************************/
#define COMPACT_READ_ON_SECONDARY 1
#define HLSM_CPREFETCH false //for compaction
#define USE_OPQ_THREAD
#define COMPACT_SECONDARY_PWRITE


#define EXCLUDE_FILE(fname_, str_)	((fname_.find(str_) == std::string::npos))
#define EXCLUDE_FILES(fname_)	((MIRROR_ENABLE ? 	\
	EXCLUDE_FILE(fname_, "MANIFEST") && EXCLUDE_FILE(fname_, "CURRENT") 	\
	&& EXCLUDE_FILE(fname_, ".dbtmp") && EXCLUDE_FILE(fname_, "LOG") 	\
	&& EXCLUDE_FILE(fname_, ".log") && EXCLUDE_FILE(fname_, "LOCK") : 0 ))

//assume that .log will only be written, deleted, and renamed
//#define HLSM_HDD_ONLY(fname_)	((MIRROR_ENABLE ? 	\
	!EXCLUDE_FILE(fname_, ".log") : 0 ))		\

#define HLSM_HDD_ONLY(fname_)	0

/************************** For Mirror Write Status ****************************/

namespace leveldb {

class FileNameHash {
#define HSIZE 4096
private:
	static uint32_t hash[HSIZE]; 

public:
	static int add(const std::string filename) {
		uint32_t h = Hash(filename.c_str(), filename.length(), 1);
		hash[h%HSIZE]++;
		return 0;
	}

	static int drop(const std::string filename) {
		uint32_t h = Hash(filename.c_str(), filename.length(), 1);
		hash[h%HSIZE]--;

		return 0;
	}

	static int inuse(const std::string filename) {
		uint32_t h = Hash(filename.c_str(), filename.length(), 1);
		return (hash[h%HSIZE]>0);
	}

#undef HSIZE
};

}

/************************** Asynchronous Mirror I/O *****************************/

//1. Status Append(const Slice& data)
//2. Status Sync() 
//3. Status Close() 
typedef enum { MAppend = 1, MSync, MClose, MDelete, MHalt, MBufSync, MBufClose, MTruncate} mio_op_t;

typedef struct {
	mio_op_t type;
	void* ptr1;
	void* ptr2;
	int fd;
	size_t size;
	uint64_t offset;
} *mio_op, mio_op_s;

struct entry_ {
	mio_op op;
	TAILQ_ENTRY(entry_) entries_;
};

typedef struct entry_ entry_s;

typedef struct {
	pthread_mutex_t mutex;
	TAILQ_HEAD(tailhead, entry_) head;

	pthread_cond_t noop; //no operation
	pthread_mutex_t cond_m;
} *opq, opq_s;

#define OPQ_MALLOC	(opq) malloc(sizeof(opq_s))

#define OPQ_NONEMPTY(q_)	(( (q_->head).tqh_first ))

#define OPQ_INIT(q_) 	do {		\
		pthread_mutex_init(&(q_->mutex), NULL);	\
		pthread_mutex_init(&(q_->cond_m), NULL);\
		pthread_cond_init(&(q_->noop), NULL);\
		TAILQ_INIT(&(q_->head));	\
	} while(0)

#define OPQ_WAIT(q_) do { \
		pthread_mutex_lock(&(q_->cond_m) );	\
		pthread_cond_wait(&(q_->noop), &(q_->cond_m) ); 	\
		pthread_mutex_unlock(&(q_->cond_m) );\
	} while(0)

#define OPQ_WAKEUP(q_) do { \
		pthread_mutex_lock(&(q_->cond_m) );	\
		pthread_cond_signal(&(q_->noop) ); \
		pthread_mutex_unlock(&(q_->cond_m) );\
	} while(0)

#define OPQ_ADD(q_, op_)	do {	\
		struct entry_ *e_;\
		e_ = (struct entry_ *) malloc(sizeof(struct entry_));	\
		e_->op = op_;	\
		pthread_mutex_lock(&(q_->mutex) );	\
		TAILQ_INSERT_TAIL(&(q_->head), e_, entries_);	\
		pthread_mutex_unlock(&(q_->mutex) );\
		OPQ_WAKEUP(q_);	\
	} while(0)

#define OPQ_ADD_TRUNCATE(q_, fd_, size_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MTruncate;\
		op_->fd = fd_;	\
		op_->size = size_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_SYNC(q_, mfp_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MSync;\
		op_->ptr1 = mfp_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_BUF_SYNC(q_, buf_, size_, fd_, off_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MBufSync;\
		op_->ptr1 = buf_;	\
		op_->size = size_;\
		op_->fd = fd_;		\
		op_->offset = off_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_CLOSE(q_, mfp_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MClose;	\
		op_->ptr1 = mfp_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_BUF_CLOSE(q_, fd_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MBufClose;\
		op_->fd = fd_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_DELETE(q_, fname_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MDelete;		\
		op_->ptr1 = (void*)fname_;	\
		OPQ_ADD(q_, op_);		\
	} while(0)

#define OPQ_ADD_HALT(q_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MHalt;		\
		OPQ_ADD(q_, op_);		\
	} while(0)

#define OPQ_ADD_APPEND(q_, mfp_, slice_)do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MAppend;	\
		op_->ptr1 = mfp_;	\
		op_->ptr2 = (void *)slice_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_POP(q_, op_) do{	\
		struct entry_ *e_;				\
		pthread_mutex_lock(&(q_->mutex) );	\
		e_ = (q_->head.tqh_first);\
		TAILQ_REMOVE(&(q_->head), (q_->head).tqh_first, entries_);\
		pthread_mutex_unlock(&(q_->mutex) );\
		op_ = e_->op;	\
		free(e_);			\
	} while(0) //ToDo: free e_

#define INIT_HELPER_AND_QUEUE(helper_, queue_)	\
	do {										\
		if (helper_ == NULL) {\
			helper_ = (pthread_t *) malloc(sizeof(pthread_t));	\
			queue_ = OPQ_MALLOC;\
			OPQ_INIT(queue_);		\
			pthread_create(helper_, NULL,  &mirrorCompactionHelper, queue_);	\
			DEBUG_INFO2("INIT_HELPER", queue_);	\
		}											\
	} while (0)

#endif  //MIRROR_LEVEL_H
