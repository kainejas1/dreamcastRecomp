#ifndef DC_SELFTEST_H
#define DC_SELFTEST_H

/* Checks the opcode table for overlaps and runs known encodings through
 * the decoder. Returns 1 on success. */
int sh4_selftest(int verbose);

#endif
