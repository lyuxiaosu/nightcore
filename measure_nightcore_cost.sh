# Please kubectl edit configmap config-autoscaler -n knative-serving with the following value
    #initial-scale: "0"
# allow-zero-initial-scale controls whether either the cluster-wide initial-scale flag,
# or the "autoscaling.knative.dev/initialScale" annotation, can be set to 0.
    #allow-zero-initial-scale: "true"

#!/bin/bash

function usage {
        echo "$0 [repeat count] [to-gateway or to-engine]"
        exit 1
}

if [ $# != 2 ] ; then
        usage
        exit 1;
fi

remote_ip="10.10.1.1"
repeat_count=$1
to=$2

chmod 400 ./id_rsa

> nightcore.log

path="/my_mount/nightcore/examples/c"
if [ "$to" = "to-engine" ]; then
    for ((i = 1; i <= $repeat_count; i++)); do
        echo "Loop iteration: $i"
        ssh -o stricthostkeychecking=no -i ./id_rsa xiaosuGW@$remote_ip "$path/run_stack.sh > 1.txt 2>&1 &"
        echo "start client..."
        ./bypass_gw_curl.sh >> nightcore.log 
        
        ssh -o stricthostkeychecking=no -i ./id_rsa xiaosuGW@$remote_ip  "$path/kill_nightcore.sh"

    done 
elif [ "$to" = "to-gateway" ]; then
    for ((i = 1; i <= $repeat_count; i++)); do
        echo "Loop iteration: $i"
        ssh -o stricthostkeychecking=no -i ./id_rsa xiaosuGW@$remote_ip "$path/run_stack.sh > 1.txt 2>&1 &"
        echo "start client..."
        ./gw_curl.sh >> nightcore.log
        ssh -o stricthostkeychecking=no -i ./id_rsa xiaosuGW@$remote_ip  "$path/kill_nightcore.sh" 
    done

fi
    
