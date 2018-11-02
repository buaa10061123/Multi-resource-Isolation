#!/bin/bash
##################
#Author quxm
#统计online组task数量
#参数1 online组目录 eg./sys/cgroup/cpuset/hadoop-yarn/docker-online/
##################
find $1 -type f -name "tasks" -exec cat {} \; | grep -v '^$' | wc -l
