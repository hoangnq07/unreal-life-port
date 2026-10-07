#ifndef OPENSLES_H
#define OPENSLES_H

void opensles_init(void);
void opensles_flush_dump(void);   /* write the rolling PCM capture to /tmp (if UNITY_DUMP_PCM=1) */

#endif
