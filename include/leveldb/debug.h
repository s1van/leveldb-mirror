#ifndef DEBUG_LEVELDB_H
#define DEBUG_LEVELDB_H

#include <iostream>
#include <sys/time.h>

//#define DEBUG_DUMP


#ifdef DEBUG_DUMP

#define PRINT_CURRENT_TIME	do {		\
		struct timeval now;		\
		gettimeofday(&now, NULL);	\
		now.tv_sec = (now.tv_sec << 36) >> 36;		\
		std::cout << now.tv_sec * 1000000 + now.tv_usec;\
	} while(0)

#define PRINT_LOC_INFO	do{	\
		std::cout << "[" << __FUNCTION__ << ",\t"	\
		<< __FILE__ << ": " << __LINE__ << "]";		\
	} while(0)

#define DEBUG_INFO(_str)  do{		\
		PRINT_CURRENT_TIME; 	\
		std::cout << "\t";	\
		PRINT_LOC_INFO;		\
		std::cout << "\t" << _str << std::endl;	\
	} while(0)

#define DEBUG_INFO2(_arg1, _arg2)  do{	\
		PRINT_CURRENT_TIME;	\
		std::cout << "\t";	\
		PRINT_LOC_INFO;		\
		std::cout << _arg1 << "\t" << _arg2 << std::endl;\
	} while(0)

#define DEBUG_INFO3(_arg1, _arg2, _arg3)  do{	\
		PRINT_CURRENT_TIME;		\
		std::cout << "\t";		\
		PRINT_LOC_INFO;			\
		std::cout << _arg1 << "\t" 	\
			<< _arg2 << "\t"	\
			<< _arg3 << std::endl;	\
	} while(0)

#define DEBUG_META_VEC(_tag, _vec) do{		\
		PRINT_CURRENT_TIME;		\
		std::cout << "\t";		\
		PRINT_LOC_INFO;			\
		std::cout << "\t" << _tag;	\
		int _i=0;					\
		for(;_i<_vec.size(); _i++){			\
			std::cout << "\t" << _vec[_i]->number;	\
		}						\
		std::cout << std::endl;		                \
	} while(0)

#else	//DEBUG_DUMP not defined

#define DEBUG_INFO(_str)
#define DEBUG_INFO2(_arg1, _arg2)
#define DEBUG_INFO3(_arg1, _arg2, _arg3)
#define DEBUG_META_VEC(_tag, _vec)

#endif  //DEBUG_DUMP

#endif  //DEBUG_LEVEL_H
