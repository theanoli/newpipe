server0="pc805"
client0="pc820"
#client1="pc730"

min_threads=1
max_threads=16
min_concurrency=2
max_concurrency=128
ntrials=3

base_config=$'--test_ms 50000
--sm_verbose 0
--batch_size 1
--msg_size 32
--num_processes 2
--numa_0_ports 0
--numa_1_ports 1'

erpc_dir="/proj/sequencer/eRPC"
app_subdir="apps/small_rpc_tput"

trial=1
while [ $trial -le $ntrials ]; do
    nthreads=$min_threads
    while [ $nthreads -le $max_threads ]; do
        concurrency=$min_concurrency

        while [ $concurrency -le $max_concurrency ]; do
            sudo pkill -9 small_rpc
            ssh -p 22 theano@$server0.emulab.net "sudo pkill -9 small_rpc"

            echo "Doing $nthreads threads and $concurrency concurrency, trial $trial"
            echo "$base_config" > $erpc_dir/$app_subdir/config
            echo --num_threads $nthreads >> $erpc_dir/$app_subdir/config
            echo --concurrency $concurrency >> $erpc_dir/$app_subdir/config
            ssh -p 22 theano@$server0.emulab.net screen -d -m 'bash -c "cd /proj/sequencer/eRPC && sudo bash scripts/do.sh 0 0"'
            #ssh -p 22 $client1 screen -d -m sudo bash /proj/sequencer/eRPC/scripts/do.sh 2 0 &
            pushd $erpc_dir
            sudo bash $erpc_dir/scripts/do.sh 1 0
            popd
           
            sleep 5
            cp /tmp/small_rpc_tput_stats_1 ./results/c"$concurrency"_t"$nthreads_$trial"

            sudo pkill -9 small_rpc
            ssh -p 22 theano@server0.emulab.net screen -d -m "sudo pkill -9 small_rpc"

            ((concurrency=$concurrency*2))
        done

        ((nthreads=$nthreads*2))
    done

    ((trial=$trial+1))
done
