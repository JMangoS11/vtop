

/*
 * Copyright (C) 2018-2019 VMware, Inc.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <limits.h>
#include <assert.h>
#include <iostream>
#include <string.h>
#include <random>
#include <vector>
#include <fstream>
#include <dirent.h>
#include <signal.h>
#include <sstream>
#include <sys/syscall.h>
#include <unordered_map>
#include <stdio.h>

#include <math.h>
#include <math.h>
#include <float.h>

#include <iostream>
#include <vector>
#include <map>
#include <tuple>

#include <list>
using namespace std;



#define PROBE_MODE	(0)
#define DIRECT_MODE	(1)

#define MAX_CPUS	(192)
#define GROUP_LOCAL	(0)
#define GROUP_NONLOCAL	(1)
#define GROUP_GLOBAL	(2)

#define NUMA_GROUP	(0)
#define PAIR_GROUP	(1)
#define THREAD_GROUP	(2)


#define min(a,b)	(a < b ? a : b)
#define LAST_CPU_ID	(min(nr_cpus, MAX_CPUS))

typedef unsigned atomic_t;


int nr_cpus;
//parameters
int verbose = 0;
int NR_SAMPLES = 4;
int SAMPLE_US = 350000;

int sleep_time = 4;
bool first_measurement = false;
static size_t nr_relax = 1;
int nr_numa_groups = 0;
int nr_pair_groups = 0;
int nr_tt_groups = 0;
int minimum_latency_4 = 0;
int threefour_latency_class = 15000;
int cpu_group_id[MAX_CPUS];
int cpu_pair_id[MAX_CPUS];
int cpu_tt_id[MAX_CPUS];
bool failed_test = false; 
int latency_valid = -1;
int nr_param = 500;

int aotChange = -6;
int aotVChange = 2;

int newTF = 0;
int push_up_counter = 0;

#define NUM_CLUSTERS 2 
#define MAX_ITERATIONS 100
typedef struct {
    int x;
    int i;
    int j;
} Point;


std::vector<std::vector<int>> numa_to_pair_arr;
std::vector<std::vector<int>> pair_to_thread_arr;
std::vector<std::vector<int>> thread_to_cpu_arr;

std::vector<int> numas_to_cpu;
std::vector<int> pairs_to_cpu;
std::vector<int> threads_to_cpu;

std::vector<std::vector<int>> top_stack;
pthread_t worker_tasks[MAX_CPUS];
pthread_mutex_t top_stack_mutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<pid_t> stopped_processes;


std::list<Point> latencies;


void addEntry(std::list<Point>& latencies, int latency, int i, int j) {
	Point itemToadd;
	itemToadd.x = latency;
	itemToadd.i = i;
	itemToadd.j = j;
    latencies.push_back(itemToadd);
}

// Function to print the entries in the list
void printList(const std::list<Point>& latencies) {
    int index = 0;
    for (Point latency : latencies) {
        std::cout << "Entry " << index << ": latency=" << latency.x << std::endl;
        index++;
    }
}



typedef struct {
    Point center;
    int count;
} Cluster;

void initialize_clusters(Cluster clusters[], list<Point>& latencies) {
	int i = 0;
    for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it) {
        if (i < NUM_CLUSTERS) {
            clusters[i].center = *it;
            clusters[i].count = 0;
	    i++;
	} else {
		break;
        }
    }
}

double distance(Point a, Point b) {
    return fabs(a.x - b.x);
}

void assign_points_to_clusters(list<Point>& latencies, Cluster clusters[], int* assignments) {
	int i = 0;
    for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it) {
        double min_distance = DBL_MAX;
        int min_index = 0;

        for (int j = 0; j < NUM_CLUSTERS; j++) {
            double d = distance(*it, clusters[j].center);
            if (d < min_distance) {
                min_distance = d;
                min_index = j;
            }
        }
        assignments[i] = min_index;
        clusters[min_index].count++;
	//printf("assignments[i] = %d, clusters[min_index] = %d \n", assignments[i], clusters[min_index].count);
	i++;
    }
}

void update_cluster_centers(list<Point> &latencies, Cluster clusters[], int* assignments) {
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        clusters[i].center.x = 0;
        clusters[i].count = 0;
    }
    int i = 0;
    for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it) {
        int cluster_index = assignments[i];
        clusters[cluster_index].center.x += it->x;
        clusters[cluster_index].count++;
	i++;
    }

    for (int i = 0; i < NUM_CLUSTERS; i++) {
        if (clusters[i].count > 0) {
            clusters[i].center.x /= clusters[i].count;
        }
    }
}

void k_means(list<Point>& latencies, Cluster clusters[], int* assignments) {
    initialize_clusters(clusters, latencies);

    for (int i = 0; i < MAX_ITERATIONS; i++) {
        assign_points_to_clusters(latencies, clusters, assignments);
        update_cluster_centers(latencies, clusters, assignments);
    }
}


//(delete) my current understanding. If i -> j = 0, and i -> k = 1, and j-> k = 0, then the groups must be the same.
int check_groups(int* assignments) {
	
	//checking if there are even two groups
	int count0 = 0;
	int count1 = 0;
	
	int i = 0;
	int g1 = 0;
	int g2 = 0;
	for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it){
		if(assignments[i] == 0) {g1 = (*it).x; count0++;}		
		if(assignments[i] == 1) {g2 = (*it).x; count1++;}
		i++;
	}
	if(count0 == 0 || count1 == 0) {
		printf("Only one group found\n");
		return 1;
	}


	int lower = 0;
	if(g2 < g1) {
		lower = 1;
	}

	//Number between highest of low group and lowest of high group
	i = 0;
	int max = 0;
	int low = INT_MAX;
	for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it){
                if(assignments[i] == lower) {
			if((*it).x > max) {max = (*it).x;}
		} else {
			if((*it).x < low) {low = (*it).x;}
		}
		i++;
        }
	newTF = (int)((max+low)/2);



	int valid = 0;
	i = 0;
        for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it){
		//printf("Here1 \n");
		if(assignments[i] == lower){
			int j = 0;
			for (std::list<Point>::iterator it2 = latencies.begin(); it2 != latencies.end(); ++it2){
				//printf("Here2 \n");
				if((*it2).i == (*it).i && assignments[j] != lower){
					int k = 0;
	        			for (std::list<Point>::iterator it3 = latencies.begin(); it3 != latencies.end(); ++it3){
						//printf("Here3 \n");
						if(((*it3).i == (*it).j && (*it3).j == (*it2).j) || ((*it3).j == (*it).j && (*it3).i == (*it2).j)) {
							//printf("found [%d][%d]\n",(*it3).i,(*it3).j);
							if(assignments[k] == lower) {
								printf("Verified [%d][%d] and [%d][%d] with [%d][%d]\n", (*it).i, (*it).j, (*it2).i, (*it2).j, (*it3).i, (*it3).j);
								valid = 1;
							}
							break;
						}
						k++;
					}
				}
				if(valid == 1) {break;}
				j++;
			}

		}
		if(valid == 1) {break;}
		i++;
	}
	if(valid == 0) {
		printf("Two Groups Verified: \n");
		return 0;
	} else {
		printf("One Group Verified\n");
		return 1;
	}

}






void giveTopologyToKernel(){
        std::string output_str = "";
        for(int j = 3;j<6;j++){
                for (int i = 0; i < LAST_CPU_ID; i++) {
                        for(int p=0;p< LAST_CPU_ID;p++){
                                if(top_stack[i][p]<j){
                                        output_str+="1";
                                }else{
                                        output_str+="0";
                                }
                        }
                        output_str+=";";
                }
                output_str+=":";
        }
	std::cout<<output_str<<"what"<<std::endl;
        std::ofstream procFile("/proc/edit_topology", std::ios::out | std::ios::trunc);
    if (procFile.is_open()) {
        procFile << output_str;
        procFile.close();
        std::cout << "Topology data written to /proc/edit_topology successfully." << std::endl;
    } else {
        std::cerr << "Error: Unable to open /proc/edit_topology for writing." << std::endl;
    }
}
bool toggle_CPU_active(int cpuNumber,bool active) { //commented all refrences
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpuNumber) + "/online";
    std::ofstream file(path);

    if (!file.is_open()) {
        std::cerr << "Error: Unable to open " << path << std::endl;
        return false;
    }
	if(active){
    	file << "1";
	}else{
		file << "0";
	}
	
    if (!file) {
        std::cerr << "Error: Failed to write to " << path << std::endl;
        return false;
        return false;
    }

    return true;
}



void enableAllCpus(){ //commetned all refrences
	std::vector<int> thread_cpu_mask;
	for(int z=1;z<LAST_CPU_ID;z++){
		//toggle_CPU_active(z,true);
	}
}


void disableStackingCpus(){ //commented all refrences
//	std::vector<std::vector<int>> cust_thread_to_cpu_arr = thread_to_cpu_arr;
	std::vector<int> has_been_disqualified(LAST_CPU_ID);
	std::vector<int> thread_cpu_mask;
	bool not_first;
	for(int z=0;z<thread_to_cpu_arr.size();z++){
		thread_cpu_mask = thread_to_cpu_arr[z];
		if(has_been_disqualified[z]){
			continue;
		}
		for(int x=0;x<thread_cpu_mask.size();x++){
				if(thread_cpu_mask[x] && z!=x){
					//toggle_CPU_active(x,false);
					has_been_disqualified[x] = 1;
				}
		}
    }
}

void moveCurrentThread() {
    pid_t tid;
    tid = syscall(SYS_gettid);
    std::string path = "/sys/fs/cgroup/hi_prgroup/cgroup.procs";
    std::ofstream ofs(path, std::ios_base::app);
    if (!ofs) {
        std::cerr << "Could not open the file\n";
        return;
    }
    ofs << tid << "\n";
    ofs.close();
}

std::string_view get_option(
    const std::vector<std::string_view>& args, 
    const std::string_view& option_name) {
    for (auto it = args.begin(), end = args.end(); it != end; ++it) {
        if (*it == option_name)
            if (it + 1 != end)
                return *(it + 1);
    }
    
    return "";
};


bool has_option(
    const std::vector<std::string_view>& args, 
    const std::string_view& option_name) {
    for (auto it = args.begin(), end = args.end(); it != end; ++it) {
        if (*it == option_name)
            return true;
    }
    
    return false;
};


void setArguments(const std::vector<std::string_view>& arguments) {
    verbose = has_option(arguments, "-v");
    
    auto set_option_value = [&](const std::string_view& option, int& target) {
        if (auto value = get_option(arguments, option); !value.empty()) {
            try {
                target = std::stoi(std::string(value));
            } catch(const std::invalid_argument&) {
                throw std::invalid_argument(std::string("Invalid argument for option ") + std::string(option));
            } catch(const std::out_of_range&) {
                throw std::out_of_range(std::string("Out of range argument for option ") + std::string(option));
            }
        }
    };
    
    set_option_value("-aot", aotChange);
    set_option_value("-aotV", aotVChange);
    set_option_value("-TF", threefour_latency_class);
    set_option_value("-s", NR_SAMPLES);
    set_option_value("-u", SAMPLE_US);
    set_option_value("-d",nr_param);
	set_option_value("-f",sleep_time);
}


typedef union {
	atomic_t x;
	char pad[1024];
} big_atomic_t __attribute__((aligned(1024)));
                                                                  
struct thread_args_t {
    cpu_set_t cpus;
    atomic_t me;
    atomic_t buddy;
    big_atomic_t* nr_pingpongs;
    atomic_t** pingpong_mutex;
    int* stoploops;
    std::vector<uint64_t> timestamps;
    pthread_mutex_t* mutex;
    pthread_cond_t* cond;
    int* flag;
	int* max_loops;
	bool* prepared;

    thread_args_t(int cpu_id, atomic_t me_value, atomic_t buddy_value,atomic_t** pp_mutex, big_atomic_t* nr_pp, int* stop_loops, pthread_mutex_t* mtx, pthread_cond_t* cond, int* flag,bool* prep, int* max_loops)
        : me(me_value), buddy(buddy_value), nr_pingpongs(nr_pp), pingpong_mutex(pp_mutex), stoploops(stop_loops), mutex(mtx), cond(cond), flag(flag), prepared(prep), max_loops(max_loops) {
        CPU_ZERO(&cpus);
        CPU_SET(cpu_id, &cpus);
    }
};



static inline uint64_t now_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * ((uint64_t)1000*1000*1000) + ts.tv_nsec;
}

static void common_setup(thread_args_t *args)
{
	if (sched_setaffinity(0, sizeof(cpu_set_t), &args->cpus)) {
		perror("sched_setaffinity");
		exit(1);
	}

	if (args->me == 0) {
		*(args->pingpong_mutex) = (atomic_t*)mmap(0, getpagesize(), PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
		if (*(args->pingpong_mutex) == MAP_FAILED) {
			perror("mmap");
			exit(1);
		}
		*(*(args->pingpong_mutex)) = args->me;
	}

	// ensure both threads are ready before we leave -- so that
	// both threads have a copy of pingpong_mutex.
	pthread_mutex_lock(args->mutex);
    if (*(args->flag)) {
        *(args->flag) = 0;
        pthread_cond_wait(args->cond, args->mutex);
    } else {
        *(args->flag) = 1;
        pthread_cond_broadcast(args->cond);
    }
    pthread_mutex_unlock(args->mutex);
	*(args->prepared) = true;
}

static void *thread_fn(void *data)
{
//	moveCurrentThread();
	int amount_of_loops = 0;
	thread_args_t *args = (thread_args_t *)data;
	common_setup(args);
	big_atomic_t *nr_pingpongs = args->nr_pingpongs;
	atomic_t nr = 0;
	bool done = false;
	atomic_t me = args->me;
	atomic_t buddy = args->buddy;
	int *stop_loops = args->stoploops;
	int *max_loops = args->max_loops;
	atomic_t *cache_pingpong_mutex = *(args->pingpong_mutex);
	while (1) {
		if((args->timestamps).size() > 100) {
			*stop_loops = 4;
                        pthread_exit(0);
                }

		if(amount_of_loops++ == *max_loops){
			//printf("SL = %d aol = %d\n", *stop_loops, amount_of_loops);
			if(*stop_loops == 1){
				*stop_loops +=3;
				//printf("case 1\n");
				pthread_exit(0);
			}else{
			   *stop_loops += 1;
			}
		}
		if (*stop_loops>2){
			//printf("case 2\n");
			pthread_exit(0);
		}


		
		if (__sync_bool_compare_and_swap(cache_pingpong_mutex, me, buddy)) {
			++nr;
			if ((nr==nr_param) && me == 0) {
				(args->timestamps).push_back(now_nsec());
				nr = 0;
			}
		}

	}
	return NULL;
}

//pins calling thread to two cores
int stick_this_thread_to_core(int core_id,int core_id2) {
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   if (core_id < 0 || core_id >= num_cores)
      return EINVAL;
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);
   CPU_SET(core_id2, &cpuset);
   pthread_t current_thread = pthread_self();    
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

int get_latency_class(int latency, int i, int j){
        if(latency<0 || latency>50000){
                return 1;
        }

        if(latency< 1500){
                return 2;
        }
        if(latency< threefour_latency_class){
		//printf("--------THREE FOUR IS %d", threefour_latency_class);
		addEntry(latencies, latency, i, j);	
		//printList(latencies);
                return 3;
        }

        return 4;
}




int measure_latency_pair(int i, int j)
{
	int sleeping_time = SAMPLE_US;
	int amount_of_times=0;
	if(latency_valid != -1 && latency_valid != 1){
                        amount_of_times = aotChange;
    }
	//In the verification stage, we make less effort to verify the stacking topology.
	if(latency_valid == 1){
		amount_of_times = aotVChange;
	}
	int max_loops = SAMPLE_US;
	if(first_measurement){
		amount_of_times = aotChange;
		max_loops = SAMPLE_US * 10;
		first_measurement = false;
	}
	
	
	while(1){
		stick_this_thread_to_core(i,j);
		atomic_t* pingpong_mutex = (atomic_t*) malloc(sizeof(atomic_t));;
		pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
		big_atomic_t nr_pingpongs;
		int stop_loops = 0;
		bool prepared = false;
		int wait_for_buddy = 1;
		thread_args_t even(i, (atomic_t)0, (atomic_t)1, &pingpong_mutex, &nr_pingpongs, &stop_loops, &wait_mutex, &wait_cond, &wait_for_buddy,&prepared,&max_loops);
		thread_args_t odd(j, (atomic_t)1, (atomic_t)0, &pingpong_mutex, &nr_pingpongs, &stop_loops, &wait_mutex, &wait_cond, &wait_for_buddy,&prepared,&max_loops);
		pthread_t t_odd;
		pthread_t t_even;
		__sync_lock_test_and_set(&nr_pingpongs.x, 0);


		if (pthread_create(&t_odd, NULL, thread_fn, &odd)) {
			printf("ERROR creating odd thread\n");
			exit(1);
		}
		if (pthread_create(&t_even, NULL, thread_fn, &even)) {
			printf("ERROR creating even thread\n");
			exit(1);
		}

		double best_sample = 1./0.;
		
		pthread_join(t_odd,NULL);
		pthread_join(t_even,NULL);
		if(even.timestamps.size() == 1){
					continue;
		}
		munmap(pingpong_mutex,getpagesize());

		if(even.timestamps.size() <2){
			if(amount_of_times<NR_SAMPLES){
				amount_of_times++;
				//max_loops = SAMPLE_US * 2;
				continue;
			}else{
				atomic_t s = __sync_lock_test_and_set(&nr_pingpongs.x, 0);
				std::cout <<"Times around:"<<amount_of_times<<"I"<<i<<" J:"<<j<<" Sample passed " << -1 << " next.\n";
				return -1;
			}
		}
		
		for(int z=0;z<even.timestamps.size() - 1;z++){
			double sample = (even.timestamps[z+1] - even.timestamps[z]) / (double)(nr_param*2);
			if (sample < best_sample){
				best_sample = sample;
			}
		}
		std::cout<<"Times around:"<<amount_of_times<<"I"<<i<<" J:"<<j<<" Sample passed " << (int)(best_sample*100) << " next.\n";
		printf("The latency between %d and %d: %d\n", i, j, (int)(best_sample * 100));
		//cout << "latencies size is " << latencies.size() << endl;

		return (int)(best_sample * 100);
	}
}






void set_latency_pair(int x,int y,int latency_class){
	top_stack[x][y] = latency_class;
	top_stack[y][x] = latency_class;
}

void apply_optimization(void){ 
	int sub_rel;
	for(int x=0;x<LAST_CPU_ID;x++){
		for(int y=0;y<LAST_CPU_ID;y++){
			sub_rel = top_stack[y][x];
			for(int z=0;z<LAST_CPU_ID;z++){
				if((top_stack[y][z]<sub_rel && top_stack[y][z]!=0)){
					

					if(top_stack[x][z] == 0){
						set_latency_pair(x,z,sub_rel);
					}else if(top_stack[x][z] != sub_rel){


						int fixed = 0;
						int save = SAMPLE_US;
                        		        SAMPLE_US *= 3;
						int lowest = INT_MAX; 
                                		for(int w = 0; w < 6; w++) {
                                        		int latency = measure_latency_pair(x,y);
							if(latency < lowest) {lowest = latency;}
							int newClass = get_latency_class(latency,x,y);
                                       			 if(top_stack[x][z] == newClass) {
								fixed =1;
								top_stack[x][y] = newClass;
 								top_stack[y][x] = newClass;
								sub_rel = newClass;
								printf("----apply---------- \n");
                                               			 break;
                                       			 }
                               			 }
                        			SAMPLE_US = save;


						if(fixed ==0) {
							int save = SAMPLE_US;	
							SAMPLE_US *=3;
							int check_yz = get_latency_class(measure_latency_pair(y,z),y,z);
							int check_xz = get_latency_class(measure_latency_pair(x,z),x,z);
							if(check_yz != top_stack[y][z] || check_xz != top_stack[x][z]) {
								printf("Topology Change, New yz = %d, new xz = %d, old yz = %d, old xz = %d \n", check_yz, check_xz, top_stack[y][z], top_stack[x][z]);	

								failed_test = true;
								return;
							} else {
								threefour_latency_class = lowest*1.05;
								printf("-----threefour is incorrect, new value is %d \n", threefour_latency_class);
                                                        	//printf("x: %d, z: %d, topstack xz %d, sub_rel: %d \n", x, z, top_stack[x][z], sub_rel);
                                                        	//printf("y: %d, z: %d, topstack yz %d, sub_rel: %d \n", y, z, top_stack[y][z], sub_rel);
                                                        	//failed_test = true;
                                                        	//return;

							}

							SAMPLE_US = save;


						}
					}
				}
			}
		}
	}
}




static void print_population_matrix(void)
{
	int i, j;

	for (i = 0; i < LAST_CPU_ID; i++) {
		for (j = 0; j < LAST_CPU_ID; j++)
			printf("%7d", (int)(top_stack[i][j]));
		printf("\n");
	}
}





int find_numa_groups(void)
{
	nr_numa_groups = 0;
	for(int i = 0;i<LAST_CPU_ID;i++){
		cpu_group_id[i] = -1;
	}
	numa_to_pair_arr = {};
	numas_to_cpu = {};
	first_measurement = true;
	for (int i = 0; i < LAST_CPU_ID; i++) {
		if(cpu_group_id[i] != -1){
			continue;
		}
		cpu_group_id[i] = nr_numa_groups;
		for (int j = 0; j < LAST_CPU_ID; j++) {
			if(cpu_group_id[j] != -1){
				continue;
			}
			if(top_stack[i][j] == 0 ){
				
				int latency = measure_latency_pair(i,j);
				set_latency_pair(i,j,get_latency_class(latency,i,j));
			}
			if(top_stack[i][j] < 4){
				cpu_group_id[j] = nr_numa_groups;
			}
		}
		nr_numa_groups++;
		std::vector<int> cpu_bitmap_group(LAST_CPU_ID);
		numa_to_pair_arr.push_back(cpu_bitmap_group);
		numas_to_cpu.push_back(i);
	}

	apply_optimization();
	return nr_numa_groups;
}

typedef struct {
	std::vector<int> pairs_to_test;
} worker_thread_args;


void ST_find_topology(std::vector<int> input){
	int minimum_latency= INT_MAX;
	for(int x=0;x<input.size();x++){
		int j = input[x] % LAST_CPU_ID;
		int i = (input[x]-(input[x]%LAST_CPU_ID))/LAST_CPU_ID;
		
		
		if(top_stack[i][j] == 0){
			int latency = measure_latency_pair(i,j);
			pthread_mutex_lock(&top_stack_mutex);
			set_latency_pair(i,j,get_latency_class(latency,i,j));
			if(latency_valid == -1){
				apply_optimization();
			}
			pthread_mutex_unlock(&top_stack_mutex);
		}

		if(latency_valid != -1 && latency_valid != top_stack[i][j]) {
			int save = SAMPLE_US;	
			int fixFail = 0;
			SAMPLE_US *= 3;
                                for(int w = 0; w < 6; w++) {
                                        int latency = measure_latency_pair(i,j);
					if(latency < minimum_latency) {minimum_latency = latency;}
                                        if(latency_valid == get_latency_class(latency,i,j)) {
						printf("--------------- \n");
						top_stack[i][j] = get_latency_class(latency,i,j);
						fixFail = 1;
                                                break;
                                        }
                                }
			SAMPLE_US = save;
			if(fixFail == 0) {
				failed_test = true;
			}
		}
		if(failed_test){


			//threefour_latency_class = minimum_latency*1.05;
                        //printf("-----threefour is incorrect, new value is %d \n", threefour_latency_class);



			printf("Failed ST, topstack[i][j] %d, i %d, j %d, latency_valid %d  \n",top_stack[i][j], i, j, latency_valid);
                        return;

		}
	}
}



static void *thread_fn2(void *data)
{
	
	worker_thread_args *args = (worker_thread_args *)data;
	ST_find_topology(args->pairs_to_test);
	return NULL;
}


void MT_find_topology(std::vector<std::vector<int>> all_pairs_to_test){ 

	worker_thread_args worker_args[all_pairs_to_test.size()];
	pthread_t worker_tasks[all_pairs_to_test.size()];
	
	for (int i = 0; i < all_pairs_to_test.size(); i++) {
		worker_args[i].pairs_to_test = all_pairs_to_test[i];
		pthread_create(&worker_tasks[i], NULL, thread_fn2, &worker_args[i]);
	}
	for (int i = 0; i < all_pairs_to_test.size(); i++) {
    		pthread_join(worker_tasks[i], NULL);
  	}
}

void performProbing(){
	find_numa_groups();
	apply_optimization();
	std::vector<std::vector<int>> all_pairs_to_test(nr_numa_groups);
	for(int i=0;i<LAST_CPU_ID;i++){
		for(int j=i+1;j<LAST_CPU_ID;j++){
			if(top_stack[i][j] == 0){
				if(cpu_group_id[i] == cpu_group_id[j]){
					all_pairs_to_test[cpu_group_id[i]].push_back(i * LAST_CPU_ID + j);
				}
			}
		}
	}
	MT_find_topology(all_pairs_to_test);
}



bool verify_numa_group(std::vector<int> input){
	std::vector<int> nums;
	for (int i = 0; i < input.size(); ++i) {
        	if (input[i] == 1) {
            		nums.push_back(i);
        	}
    	}
	for(int i=0; i < nums.size();i++){
		for(int j=i+1;j<nums.size();j++){
			int latency = measure_latency_pair(pairs_to_cpu[nums[i]],pairs_to_cpu[nums[j]]);
			if(get_latency_class(latency,i,j) != 3){
				return false;
			}
		}
	}
	return true;
}

std::vector<int> bitmap_to_ord_vector(std::vector<int> input){
	std::vector<int> ord_vector;
	for(int i=0;i<input.size();i++){
                if(input[i] == 1){
                    ord_vector.push_back(i);
                }
        }
	return ord_vector;

}


std::vector<int> bitmap_to_task_stack(std::vector<int> input,int type){

	std::vector<int> stack;
	std::vector<int> returnstack;
	for(int i=0;i<input.size();i++){
		if(input[i] == 1){
			if(type == NUMA_GROUP){
				stack.push_back(pairs_to_cpu[i]);
			}else if(type == PAIR_GROUP){
				stack.push_back(threads_to_cpu[i]);
			}else{
				stack.push_back(i);
			}
		}
	}
	for(int i=0;i<stack.size();i++){
		for(int j=i+1;j<stack.size();j++){
			returnstack.push_back(stack[i]*LAST_CPU_ID+stack[j]);
		}
	}
	return returnstack;
}





void nullify_changes(std::vector<std::vector<int>> input){
	for (int z = 0; z < input.size(); z++) {
		for (int x = 0; x < input[z].size();x++) {
			int j = input[z][x] % LAST_CPU_ID;
			int i = (input[z][x]-(input[z][x]%LAST_CPU_ID))/LAST_CPU_ID;
			set_latency_pair(i,j,0);
		}
	}

}


bool verify_topology(void){

	for(int i=0;i<LAST_CPU_ID;i++){
		for(int j=0;j<LAST_CPU_ID;j++){
			if(i==j){
				top_stack[i][j] = 1;
			}else{
				top_stack[i][j] = 0;
			}
		}
	}

	first_measurement = true;
	for(int i=0;i<nr_numa_groups;i++){
        	for(int j=i+1;j<nr_numa_groups;j++){
			int latency = measure_latency_pair(numas_to_cpu[i],numas_to_cpu[j]);
                	if(get_latency_class(latency,i,j) != 4){
				printf("Failed numa verification");
                        	return false;
                	}
		}
    }

	std::vector<std::vector<int>> task_set_arr(numa_to_pair_arr.size());
	for(int i=0;i<numa_to_pair_arr.size();i++){

		task_set_arr[i] = bitmap_to_task_stack(numa_to_pair_arr[i],NUMA_GROUP);

	}

	latency_valid = 3;
	MT_find_topology(task_set_arr);
	if(failed_test == true){
		nullify_changes(task_set_arr);
		printf("Failed cpu verification \n");
		latency_valid = -1;
		return false;
	}
	task_set_arr = std::vector<std::vector<int>>(pair_to_thread_arr.size());
	for(int i=0;i<pair_to_thread_arr.size();i++){
		task_set_arr[i] = bitmap_to_task_stack(pair_to_thread_arr[i],PAIR_GROUP);
	}
	latency_valid = 2;
	MT_find_topology(task_set_arr);
	
	if(failed_test == true){
		nullify_changes(task_set_arr);
		printf("Failed smt verification");
		latency_valid = -1;
		return false;
	}
	task_set_arr = std::vector<std::vector<int>>(pair_to_thread_arr.size()); 
	for(int i=0;i<pair_to_thread_arr.size();i++){
		std::vector<int> threads_in_pair = bitmap_to_ord_vector(pair_to_thread_arr[i]);
		for(int g=0;g<threads_in_pair.size();g++){
			int thread = threads_in_pair[g];
			std::vector<int> cpus_in_thread = bitmap_to_ord_vector(thread_to_cpu_arr[thread]);
			for(int f=0;f<cpus_in_thread.size()-1;f++){
				int i_value =  cpus_in_thread[f];
				int j_value = cpus_in_thread[f+1];
				task_set_arr[i].push_back(i_value*LAST_CPU_ID+j_value);
			}
		}
	}
	latency_valid = 1;
	MT_find_topology(task_set_arr);
	if(failed_test == true){
		nullify_changes(task_set_arr);
		printf("Failed stack verification");
		return false;
	}
	return true;
}

//this is not the cleanest code in the world, could stand to be improved
static void parseTopology(void)
{
	int i, j, count = 0;
	nr_pair_groups = 0;
	nr_tt_groups = 0;
	nr_cpus = get_nprocs();


	//clear all previous topology data(excluding numa level)
	for (i = 0; i < LAST_CPU_ID; i++){
		cpu_pair_id[i] = -1;
		cpu_tt_id[i] = -1;
	}
	pair_to_thread_arr={};
	thread_to_cpu_arr={};
	pairs_to_cpu={};
	threads_to_cpu={};


	for (i = 0; i < LAST_CPU_ID; i++) {
		if (cpu_pair_id[i] == -1){
			cpu_pair_id[i] = nr_pair_groups;
			nr_pair_groups++;
			std::vector<int> cpu_bitmap_pair(LAST_CPU_ID);
			pair_to_thread_arr.push_back(cpu_bitmap_pair);
			pairs_to_cpu.push_back(i);
		}
		
		if (cpu_tt_id[i] == -1){
			cpu_tt_id[i] = nr_tt_groups;
			nr_tt_groups++;
			std::vector<int> cpu_bitmap_tt(LAST_CPU_ID);
            		thread_to_cpu_arr.push_back(cpu_bitmap_tt);
			threads_to_cpu.push_back(i);
		}

		for (j = 0 ; j < LAST_CPU_ID; j++) {
				if (top_stack[i][j]<3 && cpu_pair_id[i] != -1){
					cpu_pair_id[j] = cpu_pair_id[i];
				}
				if (top_stack[i][j]<2 && cpu_tt_id[i] != -1){
					cpu_tt_id[j] = cpu_tt_id[i];
				}

		}
		numa_to_pair_arr[cpu_group_id[i]][cpu_pair_id[i]] = 1;
		pair_to_thread_arr[cpu_pair_id[i]][cpu_tt_id[i]] = 1;
		thread_to_cpu_arr[cpu_tt_id[i]][i] = 1;
	}
	int spaces = 0;
	for (int i = 0; i < nr_numa_groups; i++) {
		spaces=0;
		std::vector<int> pairs_in_numa =  bitmap_to_ord_vector(numa_to_pair_arr[i]);
		for(int j = 0;j<pairs_in_numa.size();j++){
			std::vector<int> threads_in_pair = bitmap_to_ord_vector(pair_to_thread_arr[pairs_in_numa[j]]);	
			for(int z=0;z<threads_in_pair.size();z++){
				std::vector<int> cpus_in_thread = bitmap_to_ord_vector(thread_to_cpu_arr[threads_in_pair[z]]); 
				spaces+=1;
				for(int y=0;y<cpus_in_thread.size();y++){
					spaces+=3;
				}
			}
		}
		if(verbose){
			std::cout<<"[";
			for(int l = 0; l<spaces-2;l++){
				std::cout<<" ";
			}
			std::cout<<"]";
		}
	}
	if(verbose){
		printf("\n");
	}
	spaces = 0;

	for (int i = 0; i < nr_numa_groups; i++) {
                spaces=0;
                std::vector<int> pairs_in_numa =  bitmap_to_ord_vector(numa_to_pair_arr[i]);
                for(int j = 0;j<pairs_in_numa.size();j++){
                        std::vector<int> threads_in_pair = bitmap_to_ord_vector(pair_to_thread_arr[pairs_in_numa[j]]); 
                        for(int z=0;z<threads_in_pair.size();z++){
                                std::vector<int> cpus_in_thread = bitmap_to_ord_vector(thread_to_cpu_arr[threads_in_pair[z]]); 
                                spaces+=1;
                                for(int y=0;y<cpus_in_thread.size();y++){
                                        spaces+=3;
                                }
                        }
					if(verbose){
					std::cout<<"[";
                	for(int l = 0; l<spaces-2;l++){
                        	std::cout<<" ";
                	}
                	std::cout<<"]";
					}
					spaces=0;
                }
        }
		if(verbose){
        	printf("\n");
		}
	for (int i = 0; i < nr_numa_groups; i++) {
                spaces=0;
                std::vector<int> pairs_in_numa =  bitmap_to_ord_vector(numa_to_pair_arr[i]);
                for(int j = 0;j<pairs_in_numa.size();j++){
                        std::vector<int> threads_in_pair = bitmap_to_ord_vector(pair_to_thread_arr[pairs_in_numa[j]]); 
                        for(int z=0;z<threads_in_pair.size();z++){
                                std::vector<int> cpus_in_thread = bitmap_to_ord_vector(thread_to_cpu_arr[threads_in_pair[z]]); 
                                if(verbose){
									std::cout<<"[";
								}
                                for(int y=0;y<cpus_in_thread.size();y++){
					printf("%2d",cpus_in_thread[y]);
					if(y!=cpus_in_thread.size()-1){
						if(verbose){
							std::cout<<" ";
						}
					}
                                }
								if(verbose){
                                	std::cout<<"]";
								}
                        }
                }
        }
		if(verbose){
        printf("\n");
		}

	printf("%d ", nr_numa_groups);	
	printf("%d ", nr_pair_groups);	
	printf("%d ", nr_tt_groups);	
	printf("\n");
}

#define CPU_ID_SHIFT		(16)
/*
 * %4 is specific to our platform.
 */
#define CPU_NUMA_GROUP(mode, i)	(mode == PROBE_MODE ? cpu_group_id[i] : i % 4)
static void configure_os_numa_groups(int mode)
{
	int i;
	unsigned long val;

	/*
	 * pass vcpu & numa group id in a single word using a simple encoding:
	 * first 16 bits store the cpu identifier
	 * next 16 bits store the numa group identifier
	 * */
	for(i = 0; i < LAST_CPU_ID; i++) {
		/* store cpu identifier and left shift */
		val = i;
		val = val << CPU_ID_SHIFT;
		/* store the numa group identifier*/
		val |= CPU_NUMA_GROUP(mode, i);
	}
}

void resetTopologyMatrix(){
	for (int i = 0; i < LAST_CPU_ID; i++) {
		for(int p=0;p< LAST_CPU_ID;p++){
			if(p!=i){
				top_stack[i][p] = 0;
			}
		}
	}
}

int main(int argc, char *argv[])
{



	uint64_t popul_laten_last = now_nsec();
	uint64_t popul_laten_now = now_nsec();
	//set program to high priority
	moveCurrentThread();
	nr_cpus = get_nprocs();

	//initialize the topology matrix
	for (int i = 0; i < LAST_CPU_ID; i++) {
		std::vector<int> cpumap(LAST_CPU_ID);
		top_stack.push_back(cpumap);
	}
	for(int p=0;p< LAST_CPU_ID;p++){
		top_stack[p][p] = 1;
	}
	
	const std::vector<std::string_view> args(argv, argv + argc);
  	setArguments(args);
	//initial probing
	enableAllCpus();
	performProbing();
	if(!failed_test){
		//giveTopologyToKernel();
		parseTopology();
		disableStackingCpus();
	}else{
		printf("Probing failed, waiting until next session\n");
	}

	while(1){
		
		//list TF threashold periodically and discard first result
		if(push_up_counter++ > 10) {
			printf("Re-adjusting TF. Discard next probe\n");
			push_up_counter = 0;
			threefour_latency_class = 30000;
		}


		if(verbose) {
			print_population_matrix();
}
		//if topology is not correct, reprobe
		if(failed_test){
			failed_test=false;
			resetTopologyMatrix();
			//enableAllCpus();
			popul_laten_last = now_nsec();
			performProbing();

			//if reprobe *is* successful, topology is right and we can update the system.
			if(!failed_test){
				if(verbose)
				printf("TOPOLOGY PROBE SUCCESSFUL.TOOK (MILLISECONDS):%lf, TF = %d\n", (now_nsec()-popul_laten_last)/(double)1000000,threefour_latency_class);

				latencies.clear();

				giveTopologyToKernel();
				parseTopology();
				//disableStackingCpus();
			}else{
				if(verbose)
				printf("TOPOLOGY PROBE FAILED.TOOK (MILLISECONDS):%lf\n", (now_nsec()-popul_laten_last)/(double)1000000);
			}
			sleep(sleep_time);
			continue;
		}
		//if topology *is* correct, verify
		//enableAllCpus();
		latency_valid = -1;
		popul_laten_last = now_nsec();

		if(verify_topology()){

			if(verbose) {
				printf("TOPOLOGY VERIFIED.TOOK (MILLISECONDS):%lf TF = %d\n", (now_nsec()-popul_laten_last)/(double)1000000, threefour_latency_class);
			}

       		 	Cluster clusters[NUM_CLUSTERS];
			cout << "latencies size is " << latencies.size() << endl;
        		int* assignments = new int[latencies.size()];
        		//int assignments[1000];
        		k_means(latencies, clusters, assignments);
			int i = 0;
    			for (std::list<Point>::iterator it = latencies.begin(); it != latencies.end(); ++it) {
               		//printf("Point %d [%d] [%d] is in cluster %d\n", (*it).x, (*it).i, (*it).j, assignments[i]);
				i++;
        		}
			if(check_groups(assignments) == 0 && latencies.size() != 0) {
				threefour_latency_class = newTF;	
				printf("Threefour = %d\n", threefour_latency_class);
			}

			latencies.clear();	
			sleep(sleep_time);
		}else{
			latency_valid = -1;
			if(verbose)
			printf("TOPOLOGY VERIFICATION FAILED.TOOK (MILLISECONDS):%lf\n", (now_nsec()-popul_laten_last)/(double)1000000);
			failed_test = true;
		}
	}		
}
