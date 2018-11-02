#!/bin/bash

####################
#Author:quxm
#传入两个参数
#参数1 为cgroup父目录路径
#参数2 为要修改的cpuset.cpus值
####################
function updateCpus()
{
    for file in `ls $1`
    do
        if [ -d $1"/"$file ]
        then
            sh -c "echo $2 > $1/$file/cpuset.cpus"
        fi
    done
    sh -c "echo $2 > $1/cpuset.cpus"
}

updateCpus $1 $2
