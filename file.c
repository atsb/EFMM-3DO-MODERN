/*  :ts=8 bk=0
 *
 * file.c:	File handling routines (Opera filesystem).
 *
 * Leo L. Schwab					9301.18
 ***************************************************************************
 *				--== RCS Log ==--
 * $Log$
 */
#include <types.h>
#include <stdint.h>
#include <mem.h>
#include <io.h>
#include <filestream.h>
#include <filestreamfunctions.h>
#include <debug.h>
#include <stdlib.h>


/***************************************************************************
 * Prototypes.
 */
void *allocloadfile(char *filename, int32 memtype, intptr_t *err_len);
void filerr(char *filename, intptr_t err);
void filedie(char *filename, intptr_t err);

extern void	closestuff (void);


/***************************************************************************
 * This handy little routine allocates a buffer that's large enough for
 * the named file, then loads that file into the buffer, and.....
 * returns a pointer to the client.  The length of the file is written to
 * the file length pointed to by err_len. On failure, err_len contains
 * a pointer-sized diagnostic string value.
 * If any of the operations fails, nothing is allocated, NULL is returned,
 * and err_len contains a pointer to a diagnostic string which can then be
 * passed to filerr() or filedie().
 */
void *
allocloadfile (filename, memtype, err_len)
char	*filename;
int32	memtype;
intptr_t *err_len;
{
	Stream	*stream;
	int32	len;
	char	*errstr;
	void	*buf;
	intptr_t result;

	buf = NULL;
	errstr = NULL;

	/*
	 * Try and load it.
	 */
	if (stream = OpenDiskStream (filename, 0)) {
		if ((len = stream->st_FileLength) > 0) {
			if (buf = ALLOCMEM (len, memtype)) {
				if (ReadDiskStream (stream, buf, len) < 0)
					errstr = "Error reading file.";
			} else
				errstr = "No memory to load file.";
		} else
			errstr = "File is empty.";
		CloseDiskStream (stream);
	} else
		errstr = "File not found.";

	/*
	 * Clean up in case of failure.
	 */
	if (errstr) {
		if (buf) {
			FREEMEM (buf, len);
			buf = NULL;
		}
		result = (intptr_t)errstr;
	}
	else
		result = len;
	if (err_len)
		*err_len = result;
	return (buf);
}


void
filerr (filename, err)
char	*filename;
intptr_t err;
{
	kprintf ("%s: %s\n", filename, (char *) (intptr_t)err);
}

void
filedie (filename, err)
char	*filename;
intptr_t err;
{
	filerr (filename, err);
	closestuff ();
	exit (20);
}
