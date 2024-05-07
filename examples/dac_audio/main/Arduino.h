typedef unsigned short uint16_t;
typedef short int16_t;
#ifndef _UINT32_T_DECLARED
#define _UINT32_T_DECLARED
typedef unsigned int uint32_t;
#endif
#ifndef _INT32_T_DECLARED
#define _INT32_T_DECLARED
typedef int int32_t;
#endif
typedef unsigned char uint8_t;
#ifndef _INT8_T_DECLARED
#define _INT8_T_DECLARED
typedef char int8_t;
#endif

// #define FPSTR(pstr_pointer) (reinterpret_cast<const __FlashStringHelper *>(pstr_pointer))
#define F(string_literal) (FPSTR(PSTR(string_literal)))
