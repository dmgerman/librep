/* streams.c -- Lisp stream handling
   Copyright (C) 1993, 1994 John Harper <john@dcs.warwick.ac.uk>
   $Id$

   This file is part of Jade.

   Jade is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   Jade is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Jade; see the file COPYING.	If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* These are the Lisp objects which are classed as streams:

   FILE: [rw]
   MARK: [rw] advance pos attribute of mark afterwards
   BUFFER: [rw] from cursor pos
   (NUMBER . STRING): [r] from the NUMBER'th char of STRING
   (STRING . ACTUAL-LENGTH): [w] to after INDEX
   (BUFFER . POS): [rw] from BUFFER, POS is advanced
   (BUFFER . t): [w] end of BUFFER
   FUNCTION: [rw] call FUNCTION, when reading FUNCTION is expected to
  		  return the next character, when writing it is called with
  		  one arg, either character or string.
   PROCESS: [w] write to the stdin of the PROCESS if it's running
   t: [w] display in status line

   Note that when using any of the three BUFFER stream types, the buffer's
   restriction is respected. */

#include "jade.h"
#include "jade_protos.h"
#define BUILD_JADE
#include "regexp/regexp.h"

#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>

#ifdef NEED_MEMORY_H
# include <memory.h>
#endif

_PR int stream_getc(VALUE);
_PR int stream_ungetc(VALUE, int);
_PR int stream_putc(VALUE, int);
_PR int stream_puts(VALUE, void *, int, bool);
_PR int stream_read_esc(VALUE, int *);

_PR void file_sweep(void);
_PR int file_cmp(VALUE, VALUE);
_PR void file_prin(VALUE, VALUE);

_PR void streams_init(void);
_PR void streams_kill(void);

static int
pos_getc(TX *tx, VALUE *pos)
{
    int c = EOF;
    long row = VROW(*pos);
    long col = VCOL(*pos);
    if(row < tx->tx_LogicalEnd)
    {
	LINE *ln = tx->tx_Lines + row;
	if(col >= (ln->ln_Strlen - 1))
	{
	    if(++row == tx->tx_LogicalEnd)
		--row;
	    else
	    {
		col = 0;
		c = '\n';
	    }
	}
	else
	    c = ln->ln_Line[col++];
    }
    *pos = make_pos(col, row);
    return c;
}

static int
pos_putc(TX *tx, VALUE *pos, int c)
{
    int rc = EOF;
    if(!read_only(tx) && pad_pos(tx, *pos))
    {
	u_char tmps[2];
	VALUE end;
	tmps[0] = (u_char)c;
	tmps[1] = 0;
	end = insert_string(tx, tmps, 1, *pos);
	if(end != LISP_NULL)
	{
	    *pos = end;
	    rc = 1;
	}
    }
    return rc;
}

static int
pos_puts(TX *tx, VALUE *pos, u_char *buf, int bufLen)
{
    if(!read_only(tx) && pad_pos(tx, *pos))
    {
	VALUE end = insert_string(tx, buf, bufLen, *pos);
	if(end != LISP_NULL)
	{
	    *pos = end;
	    return bufLen;
	}
    }
    return EOF;
}

DEFSTRING(non_resident, "Marks used as streams must be resident");
DEFSTRING(proc_not_input, "Processes are not input streams");

int
stream_getc(VALUE stream)
{
    int c = EOF;
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_input, sym_nil)))
	return(c);
    switch(VTYPE(stream))
    {
	VALUE res;
	int oldgci;

    case V_File:
	if(VFILE(stream)->name)
	    c = getc(VFILE(stream)->file);
	break;

    case V_Mark:
	if(!(VMARK(stream)->mk_Flags & MKFF_RESIDENT))
	    cmd_signal(sym_invalid_stream, list_2(stream, VAL(&non_resident)));
	else
	    c = pos_getc(VMARK(stream)->mk_File.tx, &VMARK(stream)->mk_Pos);
	break;

    case V_Buffer:
	c = pos_getc(VTX(stream), get_tx_cursor_ptr(VTX(stream)));
	break;

    case V_Cons:
	res = VCAR(stream);
	if(INTP(res) && STRINGP(VCDR(stream)))
	{
	    c = (int)VSTR(VCDR(stream))[VINT(res)];
	    if(c)
		VCAR(stream) = MAKE_INT(VINT(res) + 1);
	    else
		c = EOF;
	    break;
	}
	else if(BUFFERP(res) && POSP(VCDR(stream)))
	{
	    c = pos_getc(VTX(res), &VCDR(stream));
	    break;
	}
	else if(res != sym_lambda)
	{
	    cmd_signal(sym_invalid_stream, LIST_1(stream));
	    break;
	}
	/* FALL THROUGH */

    case V_Symbol:
	oldgci = gc_inhibit;
	gc_inhibit = TRUE;
	if((res = call_lisp0(stream)) && INTP(res))
	    c = VINT(res);
	gc_inhibit = oldgci;
	break;

#ifdef HAVE_SUBPROCESSES
    case V_Process:
    {
	cmd_signal(sym_invalid_stream, list_2(stream, VAL(&proc_not_input)));
	break;
    }
#endif

    default:
	cmd_signal(sym_invalid_stream, LIST_1(stream));
    }
    return(c);
}

/* Puts back one character, it will be read next call to streamgetc on
   this stream.
   Note that some types of stream don't actually use c, they just rewind
   pointers.
   Never call this unless you *have* *successfully* read from the stream
   previously. (few checks are performed here, I assume they were made in
   streamgetc()).  */

#define POS_UNGETC(p, tx)				\
    do {						\
	long row = VROW(p), col = VCOL(p);		\
	if(--col < 0)					\
	{						\
	    row--;					\
	    col = (tx)->tx_Lines[row].ln_Strlen - 1;	\
	}						\
	(p) = make_pos(col, row);			\
    } while(0)

int
stream_ungetc(VALUE stream, int c)
{
    int rc = FALSE;
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_input, sym_nil)))
	return(rc);
    switch(VTYPE(stream))
    {
	VALUE *ptr;
	VALUE tmp;
	int oldgci;

    case V_File:
	if(ungetc(c, VFILE(stream)->file) != EOF)
	    rc = TRUE;
	break;

    case V_Mark:
	POS_UNGETC(VMARK(stream)->mk_Pos, VMARK(stream)->mk_File.tx);
	rc = TRUE;
	break;

    case V_Buffer:
	ptr = get_tx_cursor_ptr(VTX(stream));
	POS_UNGETC(*ptr, VTX(stream));
	rc = TRUE;
	break;

    case V_Cons:
	tmp = VCAR(stream);
	if(INTP(tmp) && STRINGP(VCDR(stream)))
	{
	    VCAR(stream) = MAKE_INT(VINT(tmp) - 1);
	    rc = TRUE;
	    break;
	}
	else if(BUFFERP(tmp) && POSP(VCDR(stream)))
	{
	    ptr = &VCDR(stream);
	    POS_UNGETC(*ptr, VTX(tmp));
	    rc = TRUE;
	    break;
	}
	/* FALL THROUGH */

    case V_Symbol:
	tmp = MAKE_INT(c);
	oldgci = gc_inhibit;
	gc_inhibit = TRUE;
	if((tmp = call_lisp1(stream, tmp)) && !NILP(tmp))
	    rc = TRUE;
	gc_inhibit = oldgci;
	break;
    }
    return(rc);
}

int
stream_putc(VALUE stream, int c)
{
    int rc = 0;
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_output, sym_nil)))
	return(rc);
    switch(VTYPE(stream))
    {
	VALUE args, res, new;
	int len;
	u_char tmps[2];

    case V_File:
	if(VFILE(stream)->name)
	{
	    if(putc(c, VFILE(stream)->file) != EOF)
		rc = 1;
	}
	break;

    case V_Mark:
	if(!(VMARK(stream)->mk_Flags & MKFF_RESIDENT))
	    cmd_signal(sym_invalid_stream,
		       list_2(stream, VAL(&non_resident)));
	else
	    rc = pos_putc(VMARK(stream)->mk_File.tx, &VMARK(stream)->mk_Pos, c);
	break;

    case V_Buffer:
	rc = pos_putc(VTX(stream), get_tx_cursor_ptr(VTX(stream)), c);
	break;

    case V_Cons:
	args = VCAR(stream);
	if(STRINGP(args) && STRING_WRITABLE_P(args) && INTP(VCDR(stream)))
	{
	    int actuallen = VINT(VCDR(stream));
	    len = STRING_LEN(args);
	    if(len + 1 >= actuallen)
	    {
		int newlen = actuallen < 16 ? 32 : actuallen * 2;
		new = make_string(newlen + 1);
		if(!new)
		    break;
		memcpy(VSTR(new), VSTR(args), len);
		VCAR(stream) = new;
		VCDR(stream) = MAKE_INT(newlen);
		args = new;
	    }
	    VSTR(args)[len] = (u_char)c;
	    VSTR(args)[len+1] = 0;
	    set_string_len(args, len + 1);
	    rc = 1;
	    break;
	}
	else if(BUFFERP(args))
	{
	    if(POSP(VCDR(stream)))
		rc = pos_putc(VTX(args), &VCDR(stream), c);
	    else
	    {
		VALUE pos = cmd_restriction_end(args);
		rc = pos_putc(VTX(args), &pos, c);
	    }
	    break;
	}
	else if(args != sym_lambda)
	{
	    cmd_signal(sym_invalid_stream, LIST_1(stream));
	    break;
	}
	/* FALL THROUGH */

    case V_Symbol:
	if(stream == sym_t)
	{
	    if(curr_win->w_Flags & WINFF_MESSAGE)
	    {
		WIN *w = curr_win;
		u_char *s;
		s = str_dupn(w->w_Message, w->w_MessageLen + 1);
		if(s)
		{
		    s[w->w_MessageLen++] = c;
		    s[w->w_MessageLen] = 0;
		    str_free(w->w_Message);
		    w->w_Message = s;
		    w->w_Flags |= WINFF_MESSAGE;
		    w->w_MiniBuf->vw_Flags |= VWFF_FORCE_REFRESH;
		}
	    }
	    else
	    {
		tmps[0] = (u_char)c;
		tmps[1] = 0;
		messagen(tmps, 1);
	    }
	    rc = 1;
	}
	else
	{
	    int oldgci = gc_inhibit;
	    gc_inhibit = TRUE;
	    if((res = call_lisp1(stream, MAKE_INT(c))) && !NILP(res))
		rc = 1;
	    gc_inhibit = oldgci;
	}
	break;

#ifdef HAVE_SUBPROCESSES
    case V_Process:
	tmps[0] = (u_char)c;
	tmps[1] = 0;
	rc = write_to_process(stream, tmps, 1);
	break;
#endif

    default:
	cmd_signal(sym_invalid_stream, LIST_1(stream));
    }
    return(rc);
}

int
stream_puts(VALUE stream, void *data, int bufLen, bool isValString)
{
    u_char *buf;
    int rc = 0;
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_output, sym_nil)))
	return(rc);

    buf = isValString ? VSTR(data) : data;
    if(bufLen == -1)
	bufLen = isValString ? STRING_LEN(VAL(data)) : strlen(buf);

    switch(VTYPE(stream))
    {
	VALUE args, res, new;
	int len, newlen;

    case V_File:
	if(VFILE(stream)->name)
	{
	    if((rc = fwrite(buf, 1, bufLen, VFILE(stream)->file)) == bufLen)
		rc = bufLen;
	}
	break;

    case V_Mark:
	if(!(VMARK(stream)->mk_Flags & MKFF_RESIDENT))
	    cmd_signal(sym_invalid_stream, list_2(stream, VAL(&non_resident)));
	else
	    rc = pos_puts(VMARK(stream)->mk_File.tx, &VMARK(stream)->mk_Pos,
			  buf, bufLen);
	break;

    case V_Buffer:
	rc = pos_puts(VTX(stream), get_tx_cursor_ptr(VTX(stream)), buf, bufLen);
	break;

    case V_Cons:
	args = VCAR(stream);
	if(STRINGP(args) && STRING_WRITABLE_P(args) && INTP(VCDR(stream)))
	{
	    int actuallen = VINT(VCDR(stream));
	    len = STRING_LEN(args);
	    newlen = len + bufLen + 1;
	    if(actuallen <= newlen)
	    {
		register int tmp = actuallen < 16 ? 32 : actuallen * 2;
		if(tmp > newlen)
		    newlen = tmp;
		new = make_string(newlen + 1);
		if(!new)
		    break;
		memcpy(VSTR(new), VSTR(args), len);
		VCAR(stream) = new;
		VCDR(stream) = MAKE_INT(newlen);
		args = new;
	    }
#if 1
	    memcpy(VSTR(args) + len, buf, bufLen);
	    VSTR(args)[len + bufLen] = 0;
#else
	    strcpy(VSTR(args) + len, buf);
#endif
	    set_string_len(args, len + bufLen);
	    rc = bufLen;
	    break;
	}
	else if(BUFFERP(args))
	{
	    if(POSP(VCDR(stream)))
		rc = pos_puts(VTX(args), &VCDR(stream), buf, bufLen);
	    else
	    {
		VALUE pos = cmd_restriction_end(args);
		rc = pos_puts(VTX(args), &pos, buf, bufLen);
	    }
	    break;
	}
	else if(args != sym_lambda)
	{
	    cmd_signal(sym_invalid_stream, LIST_1(stream));
	    break;
	}
	/* FALL THROUGH */

    case V_Symbol:
	if(stream == sym_t)
	{
	    if(curr_win->w_Flags & WINFF_MESSAGE)
	    {
		WIN *w = curr_win;
		u_char *s;
		newlen = w->w_MessageLen + bufLen;
		s = str_dupn(w->w_Message, newlen);
		if(s)
		{
		    memcpy(s + w->w_MessageLen, buf, bufLen);
		    s[newlen] = 0;
		    str_free(w->w_Message);
		    w->w_Message = s;
		    w->w_MessageLen = newlen;
		    w->w_Flags |= WINFF_MESSAGE;
		    w->w_MiniBuf->vw_Flags |= VWFF_FORCE_REFRESH;
		}
	    }
	    else
		messagen(buf, bufLen);
	    rc = bufLen;
	}
	else
	{
	    int oldgci = gc_inhibit;
	    if(isValString)
		args = VAL(data);
	    else
		args = string_dupn(buf, bufLen);
	    gc_inhibit = TRUE;
	    if((res = call_lisp1(stream, args)) && !NILP(res))
	    {
		if(INTP(res))
		    rc = VINT(res);
		else
		    rc = bufLen;
	    }
	    gc_inhibit = oldgci;
	}
	break;

#ifdef HAVE_SUBPROCESSES
    case V_Process:
	rc = write_to_process(stream, buf, bufLen);
	break;
#endif

    default:
	cmd_signal(sym_invalid_stream, LIST_1(stream));
    }
    return(rc);
}

/* Read an escape sequence from STREAM. C_P should contain the first
   character of the escape *not* the escape character. Supported sequences
   are,
     n   newline
     r   carriage return
     f   form feed
     t   horizontal tab
     v   vertical tab
     a   bell
     ^C  control code of C
     012 octal character code
     x12 hex character code
   Otherwise the character is returned as-is.  */
int
stream_read_esc(VALUE stream, int *c_p)
{
    u_char c;
    switch(*c_p)
    {
    case 'n':
	c = '\n';
	break;
    case 'r':
	c = '\r';
	break;
    case 'f':
	c = '\f';
	break;
    case 't':
	c = '\t';
	break;
    case 'v':
	c = '\v';
	break;
    case 'a':
	c = '\a';
	break;
    case '^':
	c = toupper(stream_getc(stream)) ^ 0x40;
	break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
	c = *c_p - '0';
	*c_p = stream_getc(stream);
	if((*c_p >= '0') && (*c_p <= '7'))
	{
	    c = (c * 8) + (*c_p - '0');
	    *c_p = stream_getc(stream);
	    if((*c_p >= '0') && (*c_p <= '7'))
	    {
		c = (c * 8) + (*c_p - '0');
		break;
	    }
	    else
		return(c);
	}
	else
	    return(c);
    case 'x':
	c = 0;
	while(1)
	{
	    *c_p = stream_getc(stream);
	    if(!isxdigit(*c_p))
		return(c);
	    if((*c_p >= '0') && (*c_p <= '9'))
		c = (c * 16) + (*c_p - '0');
	    else
		c = (c * 16) + (toupper(*c_p) - 'A') + 10;
	}
    default:
	c = *c_p;
    }
    *c_p = stream_getc(stream);
    return(c);
}

_PR VALUE cmd_write(VALUE stream, VALUE data, VALUE len);
DEFUN("write", cmd_write, subr_write, (VALUE stream, VALUE data, VALUE len), V_Subr3, DOC_write) /*
::doc:write::
write STREAM DATA [LENGTH]

Writes DATA, which can either be a string or a character, to the stream
STREAM, returning the number of characters actually written. If DATA is
a string LENGTH can define how many characters to write.
::end:: */
{
    int actual;
    switch(VTYPE(data))
    {
	bool vstring;
	void *arg;
    case V_Int:
	actual = stream_putc(stream, VINT(data));
	break;
    case V_String:
	if(INTP(len))
	{
	    actual = VINT(len);
	    if(actual > STRING_LEN(data))
		return(signal_arg_error(len, 3));
	    if(actual == STRING_LEN(data))
	    {
		arg = VPTR(data);
		vstring = TRUE;
	    }
	    else
	    {
		arg = VSTR(data);
		vstring = FALSE;
	    }
	}
	else
	{
	    actual = STRING_LEN(data);
	    vstring = TRUE;
	    arg = VPTR(data);
	}
	actual = stream_puts(stream, arg, actual, vstring);
	break;
    default:
	return(signal_arg_error(data, 2));
    }
    return(MAKE_INT(actual));
}

_PR VALUE cmd_read_char(VALUE stream);
DEFUN("read-char", cmd_read_char, subr_read_char, (VALUE stream), V_Subr1, DOC_read_char) /*
::doc:read_char::
read-char STREAM

Reads the next character from the input-stream STREAM, if no more characters
are available returns nil.
::end:: */
{
    int rc;
    if((rc = stream_getc(stream)) != EOF)
	return(MAKE_INT(rc));
    return(sym_nil);
}

_PR VALUE cmd_read_line(VALUE stream);
DEFUN("read-line", cmd_read_line, subr_read_line, (VALUE stream), V_Subr1, DOC_read_line) /*
::doc:read_line::
read-line STREAM

Read one line of text from STREAM.
::end:: */
{
    u_char buf[400];
    if(FILEP(stream))
    {
	/* Special case for file streams. We can read a line in one go.	 */
	if(VFILE(stream)->name && fgets(buf, 400, VFILE(stream)->file))
	    return(string_dup(buf));
	return(sym_nil);
    }
    else
    {
	u_char *bufp = buf;
	int len = 0, c;
	while((c = stream_getc(stream)) != EOF)
	{
	    *bufp++ = (u_char)c;
	    if((++len >= 399) || (c == '\n'))
		break;
	}
	if(len == 0)
	    return(sym_nil);
	return(string_dupn(buf, len));
    }
}

_PR VALUE cmd_copy_stream(VALUE source, VALUE dest);
DEFUN("copy-stream", cmd_copy_stream, subr_copy_stream, (VALUE source, VALUE dest), V_Subr2, DOC_copy_stream) /*
::doc:copy_stream::
copy-stream SOURCE-STREAM DEST-STREAM

Copy all characters from SOURCE-STREAM to DEST-STREAM until an EOF is read.
::end:: */
{
#define COPY_BUFSIZ 512
    int len = 0, c;
    u_char buff[COPY_BUFSIZ];
    u_char *ptr = buff;
    while((c = stream_getc(source)) != EOF)
    {
	if(ptr - buff >= COPY_BUFSIZ - 1)
	{
	    *ptr = 0;
	    if(stream_puts(dest, buff, ptr - buff, FALSE) == EOF)
		break;
	    ptr = buff;
	}
	else
	    *ptr++ = c;
	len++;
	TEST_INT;
	if(INT_P)
	    return LISP_NULL;
    }
    if(ptr != buff)
    {
	*ptr = 0;
	stream_puts(dest, buff, ptr - buff, FALSE);
    }
    if(len)
	return(MAKE_INT(len));
    return(sym_nil);
}

_PR VALUE cmd_read(VALUE);
DEFUN("read", cmd_read, subr_read, (VALUE stream), V_Subr1, DOC_read) /*
::doc:read::
read [STREAM]

Reads one lisp-object from the input-stream STREAM (or the value of the
variable `standard-input' if STREAM is unspecified) and return it.
::end:: */
{
    VALUE res;
    int c;
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_input, sym_nil)))
    {
	signal_arg_error(stream, 1);
	return LISP_NULL;
    }
    c = stream_getc(stream);
    if(c == EOF)
	res = cmd_signal(sym_end_of_stream, LIST_1(stream));
    else
	res = readl(stream, &c);
    /* If an error occurred leave stream where it is.  */
    if(res && c != EOF)
	stream_ungetc(stream, c);
    return(res);
}

_PR VALUE cmd_print(VALUE, VALUE);
DEFUN("print", cmd_print, subr_print, (VALUE obj, VALUE stream), V_Subr2, DOC_print) /*
::doc:print::
print OBJECT [STREAM]

First outputs a newline, then prints a text representation of OBJECT to
STREAM (or the contents of the variable `standard-output') in a form suitable
for `read'.
::end:: */
{
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_output, sym_nil)))
    {
	signal_arg_error(stream, 1);
	return LISP_NULL;
    }
    stream_putc(stream, '\n');
    print_val(stream, obj);
    return(obj);
}

_PR VALUE cmd_prin1(VALUE, VALUE);
DEFUN("prin1", cmd_prin1, subr_prin1, (VALUE obj, VALUE stream), V_Subr2, DOC_prin1) /*
::doc:prin1::
prin1 OBJECT [STREAM]

Prints a text representation of OBJECT to STREAM (or the contents of the
variable `standard-output') in a form suitable for `read'.
::end:: */
{
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_output, sym_nil)))
    {
	signal_arg_error(stream, 1);
	return LISP_NULL;
    }
    print_val(stream, obj);
    return(obj);
}

_PR VALUE cmd_princ(VALUE, VALUE);
DEFUN("princ", cmd_princ, subr_princ, (VALUE obj, VALUE stream), V_Subr2, DOC_princ) /*
::doc:princ::
princ OBJECT [STREAM]

Prints a text representation of OBJECT to STREAM (or the contents of the
variable standard-output), no strange characters are quoted and no quotes
are printed around strings.
::end:: */
{
    if(NILP(stream)
       && !(stream = cmd_symbol_value(sym_standard_output, sym_nil)))
    {
	signal_arg_error(stream, 1);
	return LISP_NULL;
    }
    princ_val(stream, obj);
    return(obj);
}

_PR VALUE cmd_format(VALUE);
DEFUN("format", cmd_format, subr_format, (VALUE args), V_SubrN, DOC_format) /*
::doc:format::
format STREAM FORMAT-STRING ARGS...

Writes a string created from the format specification FORMAT-STRING and
the argument-values ARGS to the stream, STREAM. If STREAM is nil a string
is created and returned.

FORMAT-STRING is a template for the result, any `%' characters introduce
a substitution, using the next unused ARG. These format specifiers are
implemented:
   d	  print next ARG as decimal integer
   x	  print next ARG as hexadecimal integer
   o	  print next ARG in octal
   c	  print next ARG as ASCII character
   s	  unquoted representation (as from `princ') of next ARG
   S	  normal print'ed representation of next ARG
   %	  literal percentage character
::end:: */
{
    u_char *fmt, *last_fmt;
    bool mk_str;
    VALUE stream, format;
    u_char c;

    if(!CONSP(args))
	return signal_missing_arg(1);
    stream = VCAR(args);
    args = VCDR(args);
    if(NILP(stream))
    {
	stream = cmd_cons(string_dupn("", 0), MAKE_INT(0));
	mk_str = TRUE;
    }
    else
	mk_str = FALSE;


    if(!CONSP(args))
	return signal_missing_arg(2);
    format = VCAR(args);
    args = VCDR(args);
    DECLARE2(format, STRINGP);
    fmt = VSTR(format);

    last_fmt = fmt;
    while((c = *fmt++))
    {
	if(c == '%')
	{
	    if(last_fmt != fmt)
		stream_puts(stream, last_fmt, fmt - last_fmt - 1, FALSE);
	    c = *fmt++;
	    if(c == '%')
		stream_putc(stream, '%');
	    else
	    {
		u_char tbuf[40], nfmt[4];
		VALUE val;
		if(!CONSP(args))
		    return signal_missing_arg(0); /* ?? */
		val = VCAR(args);
		args = VCDR(args);
		switch(c)
		{
		case 'd': case 'x': case 'o': case 'c':
		    nfmt[0] = '%';
		    nfmt[1] = 'l';
		    nfmt[2] = c;
		    nfmt[3] = 0;
		    sprintf(tbuf, nfmt, INTP(val) ? VINT(val) : (long)val);
		    stream_puts(stream, tbuf, -1, FALSE);
		    break;

		case 's':
		    princ_val(stream, val);
		    break;

		case 'S':
		    print_val(stream, val);
		    break;
		}
	    }
	    last_fmt = fmt;
	}
    }
    if(last_fmt != fmt - 1)
	stream_puts(stream, last_fmt, fmt - last_fmt - 1, FALSE);
    if(mk_str)
    {
	if(STRING_LEN(VCAR(stream)) != VINT(VCDR(stream)))
	{
	    /* Truncate the stream to it's actual length. */
	    stream = cmd_copy_sequence(VCAR(stream));
	}
	else
	    stream = VCAR(stream);
    }
    return(stream);
}

_PR VALUE cmd_make_string_input_stream(VALUE string, VALUE start);
DEFUN("make-string-input-stream", cmd_make_string_input_stream, subr_make_string_input_stream, (VALUE string, VALUE start), V_Subr2, DOC_make_string_input_stream) /*
::doc:make_string_input_stream::
make-string-input-stream STRING [START]

Returns a input stream, it will supply, in order, the characters in STRING,
starting from START (or the beginning of the string).
::end:: */
{
    DECLARE1(string, STRINGP);
    return(cmd_cons(INTP(start) ? start : MAKE_INT(0), string));
}

_PR VALUE cmd_make_string_output_stream(void);
DEFUN("make-string-output-stream", cmd_make_string_output_stream, subr_make_string_output_stream, (void), V_Subr0, DOC_make_string_output_stream) /*
::doc:make_string_output_stream::
make-string-output-stream

Returns an output stream which will accumulate the characters written to
it for the use of the `get-output-stream-string' function.
::end:: */
{
    return(cmd_cons(string_dupn("", 0), MAKE_INT(0)));
}

_PR VALUE cmd_get_output_stream_string(VALUE strm);
DEFUN("get-output-stream-string", cmd_get_output_stream_string, subr_get_output_stream_string, (VALUE strm), V_Subr1, DOC_get_output_stream_string) /*
::doc:get_output_stream_string::
get-output-stream-string STRING-OUTPUT-STREAM

Returns a string containing the characters written to the stream STRING-
OUTPUT-STREAM (created by `make-string-output-stream'). The stream is then
reset so that the next call to this function with this stream will only
return the new characters.
::end:: */
{
    VALUE string;
    if(!CONSP(strm) || !STRINGP(VCAR(strm)) || !INTP(VCDR(strm)))
	return(signal_arg_error(strm, 1));
    if(STRING_LEN(VCAR(strm)) != VINT(VCDR(strm)))
    {
	/* Truncate the string to it's actual length. */
	string = cmd_copy_sequence(VCAR(strm));
    }
    else
	string = VCAR(strm);
    /* Reset the stream. */
    VCAR(strm) = string_dupn("", 0);
    VCDR(strm) = MAKE_INT(0);
    return(string);
}

_PR VALUE cmd_streamp(VALUE arg);
DEFUN("streamp", cmd_streamp, subr_streamp, (VALUE arg), V_Subr1, DOC_streamp) /*
::doc:streamp::
streamp ARG

Returns t if ARG is a stream.
::end:: */
{
    VALUE res = sym_nil;
    switch(VTYPE(arg))
    {
	VALUE car, cdr;
    case V_File:
    case V_Buffer:
    case V_Mark:
    case V_Process:
    case V_Symbol:
	res = sym_t;
	break;
    case V_Cons:
	car = VCAR(arg);
	cdr = VCDR(arg);
	if((car == sym_lambda)
	   || (BUFFERP(car) && (POSP(cdr) || (cdr == sym_t)))
	   || (INTP(car) && STRINGP(cdr))
	   || (STRINGP(car) && INTP(cdr)))
	    res = sym_t;
	break;
    }
    return(res);
}

static Lisp_File *lfile_chain;

void
file_sweep(void)
{
    Lisp_File *lf = lfile_chain;
    lfile_chain = NULL;
    while(lf)
    {
	Lisp_File *nxt = lf->next;
	if(!GC_CELL_MARKEDP(VAL(lf)))
	{
	    if(lf->name && !(lf->car & LFF_DONT_CLOSE))
		fclose(lf->file);
	    FREE_OBJECT(lf);
	}
	else
	{
	    GC_CLR_CELL(VAL(lf));
	    lf->next = lfile_chain;
	    lfile_chain = lf;
	}
	lf = nxt;
    }
}

int
file_cmp(VALUE v1, VALUE v2)
{
    if(VTYPE(v1) == VTYPE(v2))
    {
	if(VFILE(v1)->name && VFILE(v2)->name)
	    return(!same_files(VSTR(VFILE(v1)->name), VSTR(VFILE(v2)->name)));
    }
    return(1);
}

void
file_prin(VALUE strm, VALUE obj)
{
    stream_puts(strm, "#<file ", -1, FALSE);
    if(VFILE(obj)->name)
    {
	stream_puts(strm, VSTR(VFILE(obj)->name), -1, FALSE);
	stream_putc(strm, '>');
    }
    else
	stream_puts(strm, "*unbound*>", -1, FALSE);
}

_PR VALUE cmd_open(VALUE name, VALUE modes, VALUE file);
DEFUN("open", cmd_open, subr_open, (VALUE name, VALUE modes, VALUE file), V_Subr3, DOC_open) /*
::doc:open::
open [FILE-NAME MODE-STRING] [FILE]

Opens a file called FILE-NAME with modes MODE-STRING (standard c-library
modes, ie `r' == read, `w' == write, etc). If FILE is given it is an
existing file object which is to be closed before opening the new file on it.
::end:: */
{
    Lisp_File *lf;
    if(!FILEP(file))
    {
	lf = ALLOC_OBJECT(sizeof(Lisp_File));
	if(lf)
	{
	    lf->next = lfile_chain;
	    lfile_chain = lf;
	    lf->car = V_File;
	}
    }
    else
    {
	lf = VFILE(file);
	if(lf->name && !(lf->car & LFF_DONT_CLOSE))
	    fclose(lf->file);
    }
    if(lf != NULL)
    {
	lf->file = NULL;
	lf->name = LISP_NULL;
	lf->car = V_File;
	if(STRINGP(name) && STRINGP(modes))
	{
	    lf->file = fopen(VSTR(name), VSTR(modes));
	    if(lf->file)
	    {
		lf->name = name;
#ifdef HAVE_UNIX
		/*
		 * set close-on-exec for easy process fork()ing
		 */
		fcntl(fileno(lf->file), F_SETFD, 1);
#endif
	    }
	    else
		return(cmd_signal(sym_file_error, list_2(lookup_errno(), name)));
	}
	return(VAL(lf));
    }
    return LISP_NULL;
}

_PR VALUE cmd_close(VALUE file);
DEFUN("close", cmd_close, subr_close, (VALUE file), V_Subr1, DOC_close) /*
::doc:close::
close FILE

Kills any association between object FILE and the file in the filesystem that
it has open.
::end:: */
{
    DECLARE1(file, FILEP);
    if(VFILE(file)->name && !(VFILE(file)->car & LFF_DONT_CLOSE))
	fclose(VFILE(file)->file);
    VFILE(file)->file = NULL;
    VFILE(file)->name = LISP_NULL;
    return(file);
}

_PR VALUE cmd_flush_file(VALUE file);
DEFUN("flush-file", cmd_flush_file, subr_flush_file, (VALUE file), V_Subr1, DOC_flush_file) /*
::doc:flush_file::
flush-file FILE

Flushes any buffered output on FILE.
::end:: */
{
    DECLARE1(file, FILEP);
    if(VFILE(file)->name)
	fflush(VFILE(file)->file);
    return(file);
}

_PR VALUE cmd_filep(VALUE arg);
DEFUN("filep", cmd_filep, subr_filep, (VALUE arg), V_Subr1, DOC_filep) /*
::doc:filep::
filep ARG

Returns t if ARG is a file object.
::end:: */
{
    if(FILEP(arg))
	return(sym_t);
    return(sym_nil);
}

_PR VALUE cmd_file_bound_p(VALUE file);
DEFUN("file-bound-p", cmd_file_bound_p, subr_file_bound_p, (VALUE file), V_Subr1, DOC_file_bound_p) /*
::doc:file_bound_p::
file-bound-p FILE

Returns t if FILE is currently bound to a physical file.
::end:: */
{
    DECLARE1(file, FILEP);
    if(VFILE(file)->name)
	return(sym_t);
    return(sym_nil);
}

_PR VALUE cmd_file_binding(VALUE file);
DEFUN("file-binding", cmd_file_binding, subr_file_binding, (VALUE file), V_Subr1, DOC_file_binding) /*
::doc:file_binding::
file-binding FILE

Returns the name of the physical file FILE is bound to, or nil.
::end:: */
{
    DECLARE1(file, FILEP);
    if(VFILE(file)->name)
	return(VFILE(file)->name);
    return(sym_nil);
}

_PR VALUE cmd_file_eof_p(VALUE file);
DEFUN("file-eof-p", cmd_file_eof_p, subr_file_eof_p, (VALUE file), V_Subr1, DOC_file_eof_p) /*
::doc:file_eof_p::
file-eof-p FILE

Returns t when the end of FILE is reached.
::end:: */
{
    DECLARE1(file, FILEP);
    if(VFILE(file)->name && feof(VFILE(file)->file))
	return(sym_t);
    return(sym_nil);
}

DEFSTRING(file_unbound, "File object is unbound");

_PR VALUE cmd_read_file_until(VALUE file, VALUE re, VALUE nocase_p);
DEFUN("read-file-until", cmd_read_file_until, subr_read_file_until, (VALUE file, VALUE re, VALUE nocase_p), V_Subr3, DOC_read_file_until) /*
::doc:read_file_until::
read-file-until FILE REGEXP [IGNORE-CASE-P]

Read lines from the Lisp file object FILE until one matching the regular
expression REGEXP is found. The matching line is returned, or nil if no
lines match.
If IGNORE-CASE-P is non-nil the regexp matching is not case-sensitive.
::end:: */
{
    regexp *prog;
    u_char buf[400];		/* Fix this later. */
    DECLARE1(file, FILEP);
    DECLARE2(re, STRINGP);
    if(!VFILE(file)->name)
	return(cmd_signal(sym_bad_arg, list_2(VAL(&file_unbound), file)));
    prog = regcomp(VSTR(re));
    if(prog)
    {
	int eflags = NILP(nocase_p) ? 0 : REG_NOCASE;
	FILE *fh = VFILE(file)->file;
	VALUE res = sym_nil;
	while(fgets(buf, 400, fh))
	{
	    if(regexec2(prog, buf, eflags))
	    {
		res = string_dup(buf);
		break;
	    }
	}
	free(prog);
	return(res);
    }
    return LISP_NULL;
}

DEFSTRING(stdin_name, "<stdin>");

_PR VALUE cmd_stdin_file(void);
DEFUN("stdin-file", cmd_stdin_file, subr_stdin_file, (void), V_Subr0, DOC_stdin_file) /*
::doc:stdin_file::
stdin-file

Returns the file object representing the editor's standard input.
::end:: */
{
    static VALUE stdin_file;
    if(stdin_file)
	return(stdin_file);
    stdin_file = cmd_open(sym_nil, sym_nil, sym_nil);
    VFILE(stdin_file)->name = VAL(&stdin_name);
    VFILE(stdin_file)->file = stdin;
    VFILE(stdin_file)->car |= LFF_DONT_CLOSE;
    mark_static(&stdin_file);
    return(stdin_file);
}

DEFSTRING(stdout_name, "<stdout>");

_PR VALUE cmd_stdout_file(void);
DEFUN("stdout-file", cmd_stdout_file, subr_stdout_file, (void), V_Subr0, DOC_stdout_file) /*
::doc:stdout_file::
stdout-file

Returns the file object representing the editor's standard output.
::end:: */
{
    static VALUE stdout_file;
    if(stdout_file)
	return(stdout_file);
    stdout_file = cmd_open(sym_nil, sym_nil, sym_nil);
    VFILE(stdout_file)->name = VAL(&stdout_name);
    VFILE(stdout_file)->file = stdout;
    VFILE(stdout_file)->car |= LFF_DONT_CLOSE;
    mark_static(&stdout_file);
    return(stdout_file);
}

DEFSTRING(stderr_name, "<stderr>");

_PR VALUE cmd_stderr_file(void);
DEFUN("stderr-file", cmd_stderr_file, subr_stderr_file, (void), V_Subr0, DOC_stderr_file) /*
::doc:stderr_file::
stderr-file

Returns the file object representing the editor's standard output.
::end:: */
{
    static VALUE stderr_file;
    if(stderr_file)
	return(stderr_file);
    stderr_file = cmd_open(sym_nil, sym_nil, sym_nil);
    VFILE(stderr_file)->name = VAL(&stderr_name);
    VFILE(stderr_file)->file = stderr;
    VFILE(stderr_file)->car |= LFF_DONT_CLOSE;
    mark_static(&stderr_file);
    return(stderr_file);
}

void
streams_init(void)
{
    ADD_SUBR(subr_write);
    ADD_SUBR(subr_read_char);
    ADD_SUBR(subr_read_line);
    ADD_SUBR(subr_copy_stream);
    ADD_SUBR(subr_read);
    ADD_SUBR(subr_print);
    ADD_SUBR(subr_prin1);
    ADD_SUBR(subr_princ);
    ADD_SUBR(subr_format);
    ADD_SUBR(subr_make_string_input_stream);
    ADD_SUBR(subr_make_string_output_stream);
    ADD_SUBR(subr_get_output_stream_string);
    ADD_SUBR(subr_streamp);
    ADD_SUBR(subr_open);
    ADD_SUBR(subr_close);
    ADD_SUBR(subr_flush_file);
    ADD_SUBR(subr_filep);
    ADD_SUBR(subr_file_bound_p);
    ADD_SUBR(subr_file_binding);
    ADD_SUBR(subr_file_eof_p);
    ADD_SUBR(subr_read_file_until);
    ADD_SUBR(subr_stdin_file);
    ADD_SUBR(subr_stdout_file);
    ADD_SUBR(subr_stderr_file);
}

void
streams_kill(void)
{
    Lisp_File *lf = lfile_chain;
    while(lf)
    {
	Lisp_File *nxt = lf->next;
	if(lf->name && !(lf->car & LFF_DONT_CLOSE))
	    fclose(lf->file);
	FREE_OBJECT(lf);
	lf = nxt;
    }
    lfile_chain = NULL;
}
