#include <stdio.h>
#include <stdlib.h>
#include <time.h>

char* unnull(char* s) {
  if(!s) return "";
  return s;
}

int main() {
    struct timespec mono;
    struct timespec utc;
    struct timespec tai;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &utc);
    clock_gettime(CLOCK_TAI, &tai);
    char sloc[64];
    char sutc[64];
    char stai[64];
    time_t ml, mg;
    struct tm* tm_info;
    tm_info = localtime(&(utc.tv_sec));
    strftime(sloc, 64, "%Y-%m-%d %H:%M:%S", tm_info);
    ml = mktime(tm_info);
    int loc_hr = tm_info->tm_hour;
    tm_info = gmtime(&(utc.tv_sec));
    strftime(sutc, 64, "%Y-%m-%d %H:%M:%S", tm_info);
    mg = mktime(tm_info);
    int utc_hr = tm_info->tm_hour;
    tm_info = gmtime(&(tai.tv_sec));
    strftime(stai, 64, "%Y-%m-%d %H:%M:%S", tm_info);
    int64_t zero=0;
    printf("z_ctime:%s\n", unnull(ctime(&zero)));
    printf("l_ctime:%s\n", unnull(ctime(&ml)));
    printf("l_gtime:%s\n", unnull(ctime(&mg)));
    printf("\
mono:%d:%d\n\
loc:%s:%d\n\
utc:%s:%d\n\
tai:%s:%d\n\
ml:%d\n\
mg:%d\n\
loc_hr:%d\n\
utc_hr:%d\n\
",
	   mono.tv_sec, mono.tv_nsec,
	   sloc, utc.tv_nsec,
	   sutc, utc.tv_nsec,
	   stai, tai.tv_nsec,
	   ml,
	   mg,
	   loc_hr,
	   utc_hr);
}
