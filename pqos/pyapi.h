#include <stdio.h>
#include "../lib/pqos.h"


#ifdef __cplusplus
extern "C" {
#endif

void get_online_cores(int ips,int tasks);
int get_way_counts(int llc);