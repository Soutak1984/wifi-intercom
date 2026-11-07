#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 0 when the status page is listening. The server task owns the page buffer. */
int web_start(void);

#ifdef __cplusplus
}
#endif
