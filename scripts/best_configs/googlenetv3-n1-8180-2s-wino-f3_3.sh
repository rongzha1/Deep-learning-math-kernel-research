#/bin/bash

# googlenet_v3
# batch-size: 1
# SKX 8180 2S


source ./scripts/best_configs/common.sh $@

# googlenet_v3:conv_1_1_conv2d, ['ic', '32', 'ih', '149', 'oc', '32', 'oh', '147', 'kh', '3']
NSOCKETS=2 ./scripts/run.sh -c -i32 -h149 -o32 -H147 -n1 --blk-i=2 --blk-o=2 --blk-t=22 --tile-size=5 --execution-mode=0xa040 -p0 -P0 -v0 $COMMON (4.4T)
sleep 1
# googlenet_v3:conv_2_2_conv2d, ['ic', '32', 'ih', '147', 'oc', '64', 'oh', '147', 'kh', '3', 'ph', '1']
NSOCKETS=2 ./scripts/run.sh -c -i32 -h147 -o64 -H147 -n1 --blk-i=2 --blk-o=4 --blk-t=22 --tile-size=5 --execution-mode=0xa040 -v0 $COMMON  (5.5T)
sleep 1
# googlenet_v3:conv_4_4_conv2d, ['ic', '80', 'ih', '73', 'oc', '192', 'oh', '71', 'kh', '3']
NSOCKETS=2 ./scripts/run.sh -c -i80 -h73 -o192 -H71 -n1 --blk-i=5 --blk-o=6 --pat-o=2 --blk-t=21 --tile-size=5 --execution-mode=0xa061 --output-as-blocked=true -p0 -P0 -v0 $COMMON (7.7T)
sleep 1
# googlenet_v3:mixed_tower_1_conv_1_conv2d, ['ic', '64', 'ih', '35', 'oc', '96', 'oh', '35', 'kh', '3', 'ph', '1']
NSOCKETS=2 ./scripts/run.sh -c -i64 -h35 -o96 -H35 -n1 --blk-i=4 --blk-o=3 --blk-t=6 --pat-o=2 --tile-size=5 --execution-mode=0xa061 --output-as-blocked=true -v0 $COMMON  (3.4T)
sleep 1
# googlenet_v3:mixed_tower_1_conv_2_conv2d, ['ic', '96', 'ih', '35', 'oc', '96', 'oh', '35', 'kh', '3', 'ph', '1']
NSOCKETS=2 ./scripts/run.sh -c -i96 -h35 -o96 -H35 -n1 --blk-i=6 --blk-o=1 --blk-t=6 --pat-o=2 --tile-size=5 --execution-mode=0xa061 --output-as-blocked=true -v0 $COMMON (4.2T)
sleep 1
# googlenet_v3:mixed_9_tower_1_conv_1_conv2d, ['ic', '448', 'ih', '8', 'oc', '384', 'oh', '8', 'kh', '3', 'ph', '1']
NSOCKETS=2 ./scripts/run.sh -c -i448 -h8 -o384 -H8 -n1 --blk-i=14 --blk-o=4 --blk-t=9 --tile-size=5 --execution-mode=0xa000 --streaming-input=0 -v0 $COMMON (4.3T)
sleep 1
