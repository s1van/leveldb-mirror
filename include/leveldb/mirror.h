#ifndef MIRROR_LEVELDB_H
#define MIRROR_LEVELDB_H

#include "leveldb/debug.h"

#define MIRROR_NAME     "/data/store/db_mirror"
#define MIRROR_ENABLE	true


#define EXCLUDE_FILE(fname_, str_)	((fname_.find(str_) == std::string::npos))

#if MIRROR_ENABLE
	#define EXCLUDE_FILES(fname_)	EXCLUDE_FILE(fname_, "MANIFEST")		\
		&& EXCLUDE_FILE(fname_, "CURRENT") && EXCLUDE_FILE(fname_, ".dbtmp") 	\
		&& EXCLUDE_FILE(fname_, "LOG") && EXCLUDE_FILE(fname_, ".log")
#else
	#define EXCLUDE_FILES(fname_)	0
#endif	//MIRROR_ENABLE

#endif  //MIRROR_LEVEL_H
