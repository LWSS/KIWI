/*
	base64.c - by Joe DF (joedf@ahkscript.org)
	Released under the MIT License
	
	Revision: 2015-06-12 01:26:51
	
	Thank you for inspiration:
	http://www.codeproject.com/Tips/813146/Fast-base-functions-for-encode-decode
*/

#include <cstdint>
#include <cstdio>

//Base64 char table function - used internally for decoding
uint b64_int(uint ch);

// in_size : the number bytes to be encoded.
// Returns the recommended memory size to be allocated for the output buffer excluding the null byte
uint b64e_size(uint in_size);

// in_size : the number bytes to be decoded.
// Returns the recommended memory size to be allocated for the output buffer
uint b64d_size(uint in_size);

// in : buffer of "raw" binary to be encoded.
// in_len : number of bytes to be encoded.
// out : pointer to buffer with enough memory, user is responsible for memory allocation, receives null-terminated string
// returns size of output including null byte
uint b64_encode(const byte* in, uint in_len, byte* out);

// in : buffer of base64 string to be decoded.
// in_len : number of bytes to be decoded.
// out : pointer to buffer with enough memory, user is responsible for memory allocation, receives "raw" binary
// returns size of output excluding null byte
uint b64_decode(const byte* in, uint in_len, byte* out);

// file-version b64_encode
// Input : filenames
// returns size of output
uint b64_encodef(char *InFile, char *OutFile);

// file-version b64_decode
// Input : filenames
// returns size of output
uint b64_decodef(char *InFile, char *OutFile);