/*
 * file_io.h – File reading and writing with error handling.
 */

#ifndef OPUSEDIT_FILE_IO_H
#define OPUSEDIT_FILE_IO_H

/*
 * Open and read `filename` into the editor buffer.
 * Sets E.filename and selects syntax highlighting.
 */
void file_open(const char *filename);

/*
 * Write the entire buffer to disk. If E.filename is NULL the user
 * is prompted for a name via editor_prompt().
 */
void file_save(void);

#endif /* OPUSEDIT_FILE_IO_H */
