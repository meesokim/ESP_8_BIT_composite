typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
typedef char int8_t;

#define FPSTR(pstr_pointer) (reinterpret_cast<const __FlashStringHelper *>(pstr_pointer))
#define F(string_literal) (FPSTR(PSTR(string_literal)))
