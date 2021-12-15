#ifndef MPATHPR_H
#define MPATHPR_H

#include "structs.h" /* FILE_NAME_SIZE */

struct prin_param {
	char dev[FILE_NAME_SIZE];
	int rq_servact;
	struct prin_resp *resp;
	int noisy;
	int status;
};

struct prout_param {
	char dev[FILE_NAME_SIZE];
	int rq_servact;
	int rq_scope;
	unsigned int rq_type;
	struct prout_param_descriptor  *paramp;
	int noisy;
	int status;
};

struct threadinfo {
	int status;
	pthread_t id;
	struct prout_param param;
};

int prin_do_scsi_ioctl(char * dev, int rq_servact, struct prin_resp * resp, int noisy);
int prout_do_scsi_ioctl( char * dev, int rq_servact, int rq_scope,
		unsigned int rq_type, struct prout_param_descriptor *paramp, int noisy);
void * _mpath_pr_update (void *arg);
void dumpHex(const char* , int len, int no_ascii);

int update_prflag(char *mapname, int set);
int update_prkey_flags(char *mapname, uint64_t prkey, uint8_t sa_flags);
#define update_prkey(mapname, prkey) update_prkey_flags(mapname, prkey, 0)
void * mpath_alloc_prin_response(int prin_sa);
int update_map_pr(struct multipath *mpp);

#endif
