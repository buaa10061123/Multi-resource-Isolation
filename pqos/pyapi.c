#include "../lib/pqos.h"
#include "main.h"

#include <python3.6m/Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pyapi.h"




void get_online_cores(int ips,int tasks){
    extern int ALL_CORES[32];
    extern int OFFLINE_LLC_WAYS;
    extern int ONLINE_LLC_WAYS;
    extern int OFFLINE_MBA_PERCENT;
    extern const int LLC_WAYS;


//    PyEval_ReleaseThread(PyThreadState_Get());
//
//    PyGILState_STATE state;
//    state = PyGILState_Ensure();

    PyObject *pModule = NULL, *pDict = NULL, *pFunc = NULL, *pArg = NULL, *result = NULL;

    pModule = PyImport_ImportModule("get_best_quota");
    PyErr_Print();
    pDict = PyModule_GetDict(pModule); //获取模块字典属性 //相当于Python模块对象的__dict__ 属性，得到模块名称空间下的字典对象
    pFunc = PyDict_GetItemString(pDict, "get_mysql_quota"); //从字典属性中获取函数
    pArg = Py_BuildValue("(i, i)", ips, tasks); //参数类型转换，传递两个整型参数
    result = PyEval_CallObject(pFunc, pArg); //调用函数，并得到python类型的返回值
    float cpu,mem,llc,mba;

    PyArg_Parse(result, "(ffff)", &cpu,&mem,&llc,&mba); //将python类型的返回值转换为c/c++类型
    //ALL_CORES[0] ;
    printf("cpu=%f\n", cpu);
    printf("mem=%f\n", mem);
    printf("llc=%f\n", llc);
    printf("mba=%f\n", mba);


//    PyGILState_Release(state);

    //Py_Finalize();

    int cores = (int)cpu;
    if(cpu > cores){
        cores++;
    }
    for(int i=0;i<cores;i++){
        ALL_CORES[2*i] = 1;
    }

    OFFLINE_MBA_PERCENT = 100 - (int)mba;


    ONLINE_LLC_WAYS = (int)(llc/1024);
    if(!((int)llc%1024 == 0)){
        ONLINE_LLC_WAYS++;
    }
    int all_ways = get_way_counts(LLC_WAYS);
    OFFLINE_LLC_WAYS = all_ways - ONLINE_LLC_WAYS;
    if(OFFLINE_LLC_WAYS <= 0){
        OFFLINE_LLC_WAYS = 1;
    }

}

int get_way_counts(int llc){
    int count = 0;
    while(llc != 0){
        llc = llc >> 1;
        count++;
    }
    return count;
}

