#ifndef HLSM_AIO_WRAPPER_H 
#define HLSM_AIO_WRAPPER_H 

#include <aio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "leveldb/debug.h"
#include "port/port.h"

namespace leveldb {

class AIOWrapper {
private:
	struct aiocb cb_;
	bool finished_;
	static int prefetch_counter_;
	static port::Mutex mu_;

	void reset() {
		bzero( (char *)&cb_, sizeof(struct aiocb));
		finished_ = false;
	}
	
public:
	AIOWrapper() {
		reset();
	}

	// assume no concurrent access
	ssize_t read(int fd, void *buf, size_t count, off_t offset) {	
		int ret;

		cb_.aio_buf = buf;
		cb_.aio_fildes = fd;
		cb_.aio_nbytes = count;
		cb_.aio_offset = offset;		
		ret = aio_read( &cb_);
		if (ret < 0) perror("aio_read");
		
		mu_.Lock();
		prefetch_counter_++;
		mu_.Unlock();

		DEBUG_INFO2("aio_read", fd);
		return count; // optimistic
	}

	int wait() {
		int ret;
		while ( (ret = aio_error( &(cb_) )) == EINPROGRESS ) ;
		DEBUG_INFO3("wait aio_read", cb_.aio_fildes, ret);
		close(cb_.aio_fildes);	// file is closed after aio is completed
		
		mu_.Lock();
		prefetch_counter_--;
		mu_.Unlock();

		reset();
		finished_ = true;
		return 1;
	}

	bool isFinished() {
		return finished_;
	}

	bool isCompleted() {
		if (finished_) 
			return true;
		else 
			if ( aio_error(&(cb_)) == EINPROGRESS )
				return false;
			else
				return true;
	}

	static int getPrefetchingNum() {
		return prefetch_counter_;
	}

};


}

#endif
