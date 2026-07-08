#ifndef TYPES_H
#define TYPES_H

typedef signed char        Int8;
typedef unsigned char      UInt8;
typedef short              Int16;
typedef unsigned short     UInt16;
typedef int                Int32;
typedef unsigned int       UInt32;
typedef long long          Int64;
typedef unsigned long long UInt64;
typedef float              Float32;
typedef double             Float64;
typedef int                Bool;
typedef unsigned long long Size;

#ifndef NULL
#define NULL ((void*)0)
#endif
#define BOOL_TRUE  1
#define BOOL_FALSE 0

#endif
