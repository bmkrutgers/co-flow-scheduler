#!/bin/bash


sudo  cp pkt_sched.h  /usr/src/linux-headers-5.15.67-custom/include/uapi/linux/pkt_sched.h
sudo tc qdisc del dev enp0s3 root
sudo tc qdisc del dev lo root

sudo rmmod sch_fq

make clean

make

sudo insmod sch_fq.ko 

sudo env TC_LIB_DIR=/media/sf_Kernel-Debug/Custom-tc-novel-scheduler/scheduler/iproute2/tc ./iproute2/tc/tc help


sudo env TC_LIB_DIR=/home/bala/Documents/Custom-tc-novel-scheduler/scheduler/iproute2/tc ./iproute2/tc/tc qdisc add dev enp0s3 root fq f1_sourceport 3600 f2_sourceport 3800 f1_destport 5800 f2_destport 6000

sudo env TC_LIB_DIR=/home/bala/Documents/Custom-tc-novel-scheduler/scheduler/iproute2/tc ./iproute2/tc/tc qdisc add dev lo root fq f1_sourceport 3900 f2_sourceport 3600 f1_destport 5900 f2_destport 6800

sudo tc qdisc del dev enp0s3 root

sudo env TC_LIB_DIR=/home/bala/Documents/Custom-tc-novel-scheduler/scheduler/iproute2/tc ./iproute2/tc/tc qdisc show

#committed to new branch
