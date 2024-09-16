#!/bin/bash

rm result.txt
rm numresult.txt
./reset.sh
max=10
for i in `seq 1 $max`
do
    echo "$i th round"
    python3 UDPclient.py input_$i.txt > Input_$i.txt
    sleep 90s
    #dmesg | grep "enqueue :" | awk '{print $5}'  > refInput_$i.txt
    dmesg | grep "flow hash value in out  :"  | awk '{print $8}' | sed "s/46732/C/" | sed "s/46731/B/" | sed "s/46730/A/" | grep "[A-C]"  > Output_$i.txt
    sudo dmesg -c
    if cmp -s "Output_$i.txt" "refOutput_$i.txt"; then
    printf "Test $i has passed \n" >> result.txt
    printf "1\n" >> numresult.txt
else
    printf "Test $i has failed \n" >> result.txt
    printf "0\n" >> numresult.txt
fi
  
done


cat result.txt

arr=0 
while read line; do
    arr=$(( $arr + $line ))
done < numresult.txt

printf "$arr tests have passed \n"

b=$(($max - $arr))

printf "$b tests have failed \n"

	
sudo kill -9 $(ps -e |grep python| awk '{print $1}')

exit 0

