#include <stdio.h>

void mem_clr(char *data, int N)
{
    for(;N > 0;--N){
        *data = 0;
        data++;
    }
}

int check_sum_v1(int * data){
    unsigned int i;
    short sum = 0;
    
    for( i = 0;i < 255;++i){
        sum = (short)(data[i] + sum);
    }
    return sum;
}

int avg_test(int parama,int paramb){
    return (parama + paramb)/2;
}

int avg_test_unsigned(unsigned int parama,unsigned int paramb){
    return (parama + paramb)/2;
}

int check_sum_v2(int * data){
    unsigned int i;
    int sum = 0;
    
    for( i = 255;i != 0;--i){
        sum = data[i] + sum;
    }
    return sum;
}

int check_sum_v3(int * data){
    unsigned int i;
    int sum = 0;
    
    for( i = 255;i != 0;i--){
        sum += *(data++);
    }
    return sum;
}


int check_sum_v4(int * data, int N){
    unsigned int i;
    int sum = 0;
    
    for( i = N;i != 0;i--){
        sum += *(data++);
    }
    return sum;
}

int check_sum_v5(int * data, int N){
    unsigned int i;
    int sum = 0;
    
    do {
        sum += *(data++);
    } while(--N);
    return sum;
}