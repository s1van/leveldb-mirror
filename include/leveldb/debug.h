#ifndef DEBUG_LEVELDB_H
#define DEBUG_LEVELDB_H

#include <iostream>

//#define DEBUG_DUMP

#ifdef DEBUG_DUMP

#define DEBUG_INFO(_tag, _str)  do{std::cout << "[" << _tag << "]\t" << _str << "\n";} while(0)
#define DEBUG_INFO2(_tag, _arg1, _arg2)  do{std::cout << "[" << _tag << "]\t" << _arg1 << "\t" << _arg2 << "\n";} while(0)

#define DEBUG_META_VEC(_tag, _vec)	\
		do{ int _i=0;				\
			std::cout << "[" << _tag << "]";	\
			for(;_i<_vec.size(); _i++){			\
				std::cout << "\t" << _vec[_i]->number;	\
			}						\
			std::cout << "\n";		\
		} while(0)

#else

#define DEBUG_INFO(_tag, _str)
#define DEBUG_INFO2(_tag, _arg1, _arg2)
#define DEBUG_META_VEC(_tag, _vec)

#endif  //DEBUG_DUMP

#endif  //DEBUG_LEVEL_H
