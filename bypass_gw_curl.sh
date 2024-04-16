curl -w "Total time: %{time_total}s\n" -o /dev/null -s -X POST http://10.10.1.1:8081/function/Foo http://10.10.1.1:8081/function/Foo 
