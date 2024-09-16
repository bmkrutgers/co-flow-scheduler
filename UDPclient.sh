#!/bin/bash

#rm result.txt
./reset.sh

max=10
for i in `seq 1 $max`
do
    echo "$i th round"
    python3 UDPclient.py input_$i.txt > Input_$i.txt
    sleep 90s
    echo "$i round" >> dmesg
    dmesg > dmesg_$i.txt
    cat dmesg_$i.txt | grep "enqueue :" | awk '{print $4}'  > refInput_$i.txt
    dmesg | grep "flow hash value in out  :"  | awk '{print $9}' | sed "s/46732/C/" | sed "s/46731/B/" | sed "s/46730/A/" | grep "[A-C]"  > Output_$i.txt
    sudo dmesg -c
    cp Output_$i.txt refOutput_$i.txt
    #sudo pkill -P $$
    #if cmp -s "Output_$i.txt" "refOutput_$i.txt"; then
    #printf "Test $i has passed : 1 \n" >> result.txt
#else
#    printf "Test $i has failed : 0 \n" >> result.txt
#fi
  
done


#sudo pkill -P $$

exit 0

