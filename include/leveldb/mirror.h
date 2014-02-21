#ifndef MIRROR_LEVELDB_H
#define MIRROR_LEVELDB_H

#include "leveldb/debug.h"
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

extern int MIRROR_ENABLE;
extern const char *MIRROR_PATH;

/************************** Configuration Macros *****************************/
#define COMPACT_READ_ON_SECONDARY 0
#define HLSM_CPREFETCH false //for compaction
#define USE_OPQ_THREAD


#define EXCLUDE_FILE(fname_, str_)	((fname_.find(str_) == std::string::npos))
#define EXCLUDE_FILES(fname_)	((MIRROR_ENABLE ? 	\
	EXCLUDE_FILE(fname_, "MANIFEST") && EXCLUDE_FILE(fname_, "CURRENT") 	\
	&& EXCLUDE_FILE(fname_, ".dbtmp") && EXCLUDE_FILE(fname_, "LOG") 	\
	&& EXCLUDE_FILE(fname_, ".log") && EXCLUDE_FILE(fname_, "LOCK") : 0 ))

//assume that .log will only be written, deleted, and renamed
//#define HLSM_HDD_ONLY(fname_)	((MIRROR_ENABLE ? 	\
	!EXCLUDE_FILE(fname_, ".log") : 0 ))		\

#define HLSM_HDD_ONLY(fname_)	0

/************************** Asynchorous Mirror I/O *****************************/

//1. Status Append(const Slice& data)
//2. Status Sync() 
//3. Status Close() 
typedef enum { MAppend = 1, MSync, MClose, MDelete, MHalt} mio_op_t;

typedef struct {
	mio_op_t type;
	void* ptr1;
	void* ptr2;
} *mio_op, mio_op_s;

struct entry_ {
	mio_op op;
	TAILQ_ENTRY(entry_) entries_;
};

typedef struct entry_ entry_s;

typedef struct {
	pthread_mutex_t mutex;
	TAILQ_HEAD(tailhead, entry_) head;
} *opq, opq_s;

#define OPQ_MALLOC	(opq) malloc(sizeof(opq_s))

#define OPQ_NONEMPTY(q_)	(( (q_->head).tqh_first ))

#define OPQ_INIT(q_) 	do {		\
		pthread_mutex_init(&(q_->mutex), NULL);	\
		TAILQ_INIT(&(q_->head));	\
	} while(0)

#define OPQ_ADD(q_, op_)	do {	\
		struct entry_ *e_;	\
		e_ = (struct entry_ *) malloc(sizeof(struct entry_));	\
		e_->op = op_;		\
		pthread_mutex_lock(&(q_->mutex) );		\
		TAILQ_INSERT_TAIL(&(q_->head), e_, entries_);	\
		pthread_mutex_unlock(&(q_->mutex) );		\
	} while(0)

#define OPQ_ADD_SYNC(q_, mfp_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MSync;	\
		op_->ptr1 = mfp_;	\
		OPQ_ADD(q_, op_);	\
	} while(0)

#define OPQ_ADD_CLOSE(q_, mfp_)	do{	\
		mio_op op_ = (mio_op)malloc(sizeof(mio_op_s));	\
		op_->type = MClose;	\
		op_->ptr1 = mfp_;	\
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
