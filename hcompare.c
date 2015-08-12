/*
 * Walk a directory and give MD5 checksums of all files
 *
 * Compile with: gcc -o hcompare hcompare.c
 *
 * main git repo:  https://hzqmrk@bitbucket.org:443/hzqmrk/hcompare
 *                 ssh://hzqmrk@bitbucket.org/hzqmrk/hcompare - does not work
 *
 * TODO Properly argument inputs.
 * TODO check return values and add better comments, function headers, etc...
 * TODO does this give a useful return value (non-zero) on errors?
 * TODO Test directory input value to ensure it is a directory?
 * TODO if delimiters in ref file are not tabs it stack dumps
 * TODO full level of tests
 * TODO sequence err return numbers
 * TODO ensure read buffer size is multiple of 64
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

enum {
	WALK_OK = 0, WALK_BADPATTERN, WALK_NAMETOOLONG, WALK_BADIO,
};

#define WS_RECURSIVE	(1 << 0)		// walker library recursive bit
#define WS_DEFAULT		WS_RECURSIVE	// walker library defaults
#define WS_FOLLOWLINK	(1 << 1)		// follow symlinks
#define WS_MATCHDIRS	(1 << 3)		// if pattern is used on dir names too
#define MD5_MAX 		(32)			// max length of MD5 field
#define LENGTH_MAX 		(19)			// max length of file length field
#define LINE_LENGTH_MAX	(FILENAME_MAX+3+MD5_MAX+LENGTH_MAX)	// filename max + 3 chars for tabs/newline + md5 max + length max
#define READ_BUFFER		(1024 * 1024 * 5)	// maximum chunk to read from file during MD5 operation

// Constants are the integer part of the sines of integers (in radians) * 2^32.
const uint32_t k[64] = { 0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
		0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
		0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
		0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
		0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681,
		0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5,
		0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
		0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0,
		0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };

// r specifies the per-round shift amounts
const uint32_t r[] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17,
		22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16,
		23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
		15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

// leftrotate macro function definition
#define LEFTROTATE(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static char mflag = 'c'; 		// mode flag
static char *fvalue = NULL; 	// file to use or create
static char *dvalue = NULL; 	// directory to use
static FILE * ref_fd = NULL; 	// reference file handle
static int verbosity = 0; 		// verbosity level
static void * p_read; 			// global MD5 read buffer

#define MAX_ERR_MSG_BUFFER	(1024)		// max size of error message
static char g_ebuf[MAX_ERR_MSG_BUFFER + 1];	// error message buffer + 1 for termination

static int g_rdbufsz = READ_BUFFER;

/* @brief Write out an error message and then exit if needed
 *
 * Format is,
 *
 *   ERROR: <msg> <cr>
 *
 * @param val return value to use if exiting
 * @param msg message to send
 */
void err(int val, const char * msg) {
	printf("ERROR: %s\n", msg);
	exit(val);
}

/* @brief Write out an error message
 *
 * Format is,
 *
 *   WARN: <msg> <cr>
 *
 * @param msg message to send
 */
void warn(const char * msg) {
	printf("WARN: %s\n", msg);
}

/* @brief Write out an debug message
 *
 * Format is,
 *
 *   DEBUG: <msg> <cr>
 *
 * @param msg message to send
 */
void debug(const char * msg) {
	if (verbosity > 0){
		printf("DEBUG: %s\n", msg);
	}
}

/* @brief Store integer  in same order no matter the system byte ordering
 *
 * @param val integer value to store
 * @param  pointer to the byte array for storage
 */
void to_bytes(uint32_t val, uint8_t *bytes) {
	bytes[0] = (uint8_t) val;
	bytes[1] = (uint8_t) (val >> 8);
	bytes[2] = (uint8_t) (val >> 16);
	bytes[3] = (uint8_t) (val >> 24);
}

/* @brief Retrieve integer bytes in same order no matter the system byte ordering
 *
 * @param bytes pointer to the byte array to retrieve from
 * @return return retrieved integer
 */
uint32_t to_int32(uint8_t *bytes) {
	return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8)
			| ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

/* @brief Get MD5 of a given file
 *
 * We don't compute file length from the file open used in this function
 * as providing the length makes more sense for both computing the reference
 * file and reading/using it from the reference file.
 *
 * @param pointer to path and file name of file we want to hash (MD5)
 * @param initial_len length of the file
 * @param digest pointer for where to put the results
 */
void md5_file(char * fn, size_t initial_len, uint8_t *digest) {

	// These vars will contain the hash
	uint32_t h0, h1, h2, h3;

	// Message (to prepare)
	uint8_t *msg = NULL;

	size_t new_len, offset;
	uint32_t w[16];
	uint32_t a, b, c, d, i, f, g, temp;
	FILE * p_file;
	int n_bytes;

	// Initialize variables - simple count in nibbles
	h0 = 0x67452301;
	h1 = 0xefcdab89;
	h2 = 0x98badcfe;
	h3 = 0x10325476;

	// open the input file in posix compliant binary read mode
	p_file = fopen(fn, "rb");
	if (p_file == NULL) {
		snprintf(g_ebuf, MAX_ERR_MSG_BUFFER, "Cannot open %s to read\n", fn);
		err(1, g_ebuf);
	}

	while (!feof(p_file)) {

		// read up to READ_BUFFER bytes into our buffer
		n_bytes = fread(p_read, sizeof(uint8_t), g_rdbufsz, p_file);

		// test for read error
		if (ferror(p_file)) {
			clearerr(p_file);
			err(1, "File read error");
		}

		//Add Verbosity
		//printf("%i bytes read from %s\n", n_bytes, fn);

		msg = p_read;

		// terminate MD5 properly
		if (feof(p_file)) {
			//Pre-processing:
			//append "1" bit to message
			//append "0" bits until message length in bits = 448 (mod 512)
			//append length mod (2^64) to message
			// count on integer math
			new_len = 64 * (((int) n_bytes + 64 + 8) / 64) - 8;

			msg[n_bytes] = 0x80; // append the "1" bit; most significant bit is "first"
			for (offset = n_bytes + 1; offset < new_len; offset++)
				msg[offset] = 0; // append "0" bits

			// append the len in bits at the end of the buffer.
			to_bytes(initial_len * 8, msg + new_len);
			// initial_len>>29 == initial_len*8>>32, but avoids overflow.
			to_bytes(initial_len >> 29, msg + new_len + 4);
		} else {
			new_len = n_bytes;
		}

		// Process the message in successive 512-bit chunks:
		// for each 512-bit chunk of message:
		for (offset = 0; offset < new_len; offset += (512 / 8)) {

			// break chunk into sixteen 32-bit words w[j], 0 <= j <= 15
			for (i = 0; i < 16; i++)
				w[i] = to_int32(msg + offset + i * 4);

			// Initialize hash value for this chunk:
			a = h0;
			b = h1;
			c = h2;
			d = h3;

			// Main loop:
			for (i = 0; i < 64; i++) {

				if (i < 16) {
					f = (b & c) | ((~b) & d);
					g = i;
				} else if (i < 32) {
					f = (d & b) | ((~d) & c);
					g = (5 * i + 1) % 16;
				} else if (i < 48) {
					f = b ^ c ^ d;
					g = (3 * i + 5) % 16;
				} else {
					f = c ^ (b | (~d));
					g = (7 * i) % 16;
				}

				temp = d;
				d = c;
				c = b;
				b = b + LEFTROTATE((a + f + k[i] + w[g]), r[i]);
				a = temp;

			}

			// Add this chunk's hash to result so far:
			h0 += a;
			h1 += b;
			h2 += c;
			h3 += d;

		}

	}

	//var char digest[16] := h0 append h1 append h2 append h3 //(Output is in little-endian)
	to_bytes(h0, digest);
	to_bytes(h1, digest + 4);
	to_bytes(h2, digest + 8);
	to_bytes(h3, digest + 12);

	// close input file
	if (0 != fclose(p_file)) {
		perror("md5_file: error closing file ");
	}

}

/* @brief Walk the directory computing MD5 as we go
 *
 * @param dname pointer to the directory name
 * @param spec walking config bits
 */
int walk_recur(char *dname, int spec) {
	struct dirent *dent;
	DIR *dir;
	struct stat st;
	char fn[FILENAME_MAX];
	int res = WALK_OK;
	uint8_t result[16];
	int len = strlen(dname);
	int i = 0;
	uint8_t* p = NULL;
	void* p_buf = NULL;
	char buf[LINE_LENGTH_MAX];

	if (len >= FILENAME_MAX - 1){
		debug("walk_recur: Filename too long");
		return WALK_NAMETOOLONG;
	}
	// strcpy has no real return value
	strcpy(fn, dname);
	fn[len++] = '/';

	if (!(dir = opendir(dname))) {
		debug("walk_recur: Bad File I/O");
		return WALK_BADIO;
	}

	errno = 0;
	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
			continue;

		strncpy(fn + len, dent->d_name, FILENAME_MAX - len);
		if (lstat(fn, &st) == -1) {
			warn("Can't stat file");
			res = WALK_BADIO;
			continue;
		}

		/* don't follow symlink unless told so */
		if (S_ISLNK(st.st_mode) && !(spec & WS_FOLLOWLINK))
			continue;

		/* will be false for symlinked dirs */
		if (S_ISDIR(st.st_mode)) {
			/* recursively follow dirs */
			if ((spec & WS_RECURSIVE))
				walk_recur(fn, spec);

			if (!(spec & WS_MATCHDIRS))
				continue;
		}

		/* if it is a file then process it */
		if (!S_ISDIR(st.st_mode)) {
			md5_file(fn, st.st_size, result);

			// store to reference file
			if ((ref_fd != NULL) || (verbosity > 1)) {
				p = &result[0];
				p_buf = &buf[0];
				for (i = 0; i < 16; i++) {
					p_buf += sprintf((char *) p_buf, "%02x", *p++);
				}
				sprintf((char *) p_buf, "\t%i\t%s\n", (int) st.st_size, fn);
			}

			if (ref_fd != NULL) {
				fprintf(ref_fd, "%s", &buf[0]);
			}

			if (verbosity > 1) {
				printf("%s", &buf[0]);
			}
		}
	}

	if (dir) {
		closedir(dir);
	}

	return res ? res : errno ? WALK_BADIO : WALK_OK;
}

/* @brief parse the command line arguments with getopt
 *
 */
int parse_command_line(int argc, char **argv) {

	int rval = 1;	// return value
	int index = 0;
	int c = 0;	// getopt return value

	g_rdbufsz = READ_BUFFER;

	opterr = 0;

	while ((c = getopt(argc, argv, "m:b:f:d:v")) != -1)
		switch (c) {
		case 'm':
			mflag = *optarg;
			break;
		case 'b':
			g_rdbufsz = atoi(optarg);
			break;
		case 'd':
			dvalue = optarg;
			break;
		case 'f':
			fvalue = optarg;
			break;
		case 'v':
			verbosity++;
			break;
		case '?':
			if ((optopt == 'd') || (optopt == 'f'))
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			else if (isprint(optopt)) {
				fprintf(stderr, "Unknown option `-%c'.\n", optopt);
			} else {
				fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
			}
			rval = 0;
			break;
		default:
			rval = 0;
		}


	// list out all arguments we don't know about
	for (index = optind; index < argc; index++)
		printf("Non-option argument %s\n", argv[index]);

	return rval;
}

/* @brief Print the usage
 *
 */
void usage(void) {
	printf("Usage: hcompare [-m <Mode>] [-b <Buffer>] [-f <File>] [-d <Directory>] [-v <Verbosity>]\n");
	printf("Options:\n");
	printf(" -m <Mode>      : c = Creation (Default)\n");
	printf("                  m = Manufacturing\n");
	printf("                  a = Analysis\n");
	printf(" -b <Buffer>    : Buffer Size. Default is ()\n");
	printf(" -f <File>      : File name to be generated or read in. (Example: /fs/mmc1/ref.txt)\n");
	printf(" -d <Directory> : Directory to walk and generate MD5s for (Example: /fs/mmc1)\n");
	printf(" -v <Verbosity> : More v's More Verbose yo\n");
}

/* @brief Create an MD5 reference file
 *
 */
void create_reference_file(void) {
	int r = 0;
	debug("create_reference_file");

	debug(fvalue);

	//Open File from path for read/write mode.
	ref_fd = fopen(fvalue, "w+");
	if (NULL == ref_fd) {
		debug("create_reference_file: Cannot open reference file for write");
		err(1, "Cannot open reference file for write");
	}

	//Walk along specified path and create MD5 file.
	r = walk_recur(dvalue, WS_DEFAULT | WS_MATCHDIRS);

	if (NULL != ref_fd) {
		if (fclose(ref_fd)) {
			err(1, "error closing reference file");
		}
	}

	switch (r) {
	case WALK_OK:
		break;
	case WALK_BADIO:
		err(1, "IO error");
		break;
	case WALK_BADPATTERN:
		err(1, "Bad pattern");
		break;
	case WALK_NAMETOOLONG:
		err(1, "Filename too long");
		break;
	default:
		err(1, "Unknown walker error?");
	}
}

/* @brief Verify from reference file
 *
 */
int do_file_verification() {
	int rval = 0;					// return value
	char buf[LINE_LENGTH_MAX];		// reference file line read buffer
	char fn_buf[FILENAME_MAX];		// file name buffer
	char md5_buf[MD5_MAX];			// md5 result buffer
	char len_buf[LENGTH_MAX];		// length field read buffer
	int flen = 0;					// current file length
	char * p_c;						//
	uint8_t* p = NULL;
	void* p_buf = NULL;
	int r = 0;
	int i = 0;
	uint8_t result[16];

	debug("do_file_verification\n");

	//Open reference file
	debug("do_file_verification: Open reference file");
	ref_fd = fopen(fvalue, "r");

	//If file cannot be open, throw error.
	if (NULL == ref_fd) {
		err(1, "Cannot open reference file for read");
	}

	//Grab lines from reference file.
	while (fgets(buf, LINE_LENGTH_MAX, ref_fd)) {

		debug(buf);

		//p_c = First token in buffer string (MD5)
		p_c = strtok(buf, "\t");
		strcpy(md5_buf, p_c);

		//Grab other token (File Length)
		p_c = strtok(NULL, "\t");
		strcpy(len_buf, p_c);

		//
		sscanf(len_buf, "%i", &flen);

		//Grab last token (File Name)
		p_c = strtok(NULL, "\n");
		strcpy(fn_buf, p_c);

		//Run MD5 passing in Filename and File Length.
		md5_file(fn_buf, flen, result);

		p = &result[0];
		p_buf = &buf[0];
		for (i = 0; i < 16; i++) {
			p_buf += sprintf((char *) p_buf, "%02x", *p++);
		}

		//Compare strings together... MAX 16 char
		r = strncmp(md5_buf, buf, 16);

		if (r != 0) {
			//mflag m = Manufacter Mode
			if (mflag == 'm'){
				printf("ERROR: %s\n", fn_buf);
				err(3, "Error File Mismatch");
			//mflag a = Analysis Mode
			}else if (mflag == 'a'){
				warn(fn_buf);
				rval = 1;
			}
		}
	}

	return rval;
}

int main(int argc, char **argv) {
	int r = -1;	// return code

	// parse the command line
	if (parse_command_line(argc, argv) == 0) {
		usage();
		debug("main: command line errors");
		err(1, "command line errors");
	}

	// make a large read buffer for MD5 operations
	// adding 64 bytes for padding and md5 length
	p_read = malloc(g_rdbufsz + 64);
	if (!p_read) {
		debug("main: malloc errors");
		err(2, "malloc errors");
	}

	// create reference file or verify files in reference file
	if (mflag == 'c') {
		create_reference_file();
	} else {
		r = do_file_verification();
	}

	// give the return value back to caller
	return r;
}
