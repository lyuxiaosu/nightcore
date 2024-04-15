#!/bin/bash

pid=`ps -ef|grep  "nightcore"|grep -v grep |awk '{print $2}'`
echo $pid
sudo kill -9 $pid
