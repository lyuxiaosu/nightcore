ulimit -n 655350
curl -I -w "Total time: %{time_total}s TCP Connect time: %{time_connect}s\n" -o /dev/null -s -X POST http://10.10.1.1:8081/function/Foo http://10.10.1.1:8081/function/Foo 
#hey -disable-compression -disable-redirects -cpus 15 -z "20"s -c "60" -m POST "http://10.10.1.1:8081/function/Foo" 

