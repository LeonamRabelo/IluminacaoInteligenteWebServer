#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "inc/ssd1306.h"
#include "inc/font.h"
#include "ws2812.pio.h"

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#define WIFI_SSID "PRF"
#define WIFI_PASSWORD "@hfs0800"

//Definição de GPIOs
#define JOYSTICK_X 26  //ADC0
#define JOYSTICK_Y 27  //ADC1
#define BOTAO_A 5       //Pino do botão A
#define BOTAO_B 6       //Pino do botão B
#define WS2812_PIN 7    //Pino do WS2812
#define LED_RED 13      //Pino do LED vermelho
#define BUZZER_PIN 21   //Pino do buzzer
#define I2C_SDA 14      //Pino SDA - Dados
#define I2C_SCL 15      //Pino SCL - Clock
#define IS_RGBW false   //Maquina PIO para RGBW
#define NUM_PIXELS 25   //Quantidade de LEDs na matriz
#define NUM_NUMBERS 11  //Quantidade de numeros na matriz

//Variável global para armazenar a cor (Entre 0 e 255 para intensidade)
uint8_t led_r = 20; //Intensidade do vermelho
uint8_t led_g = 20; //Intensidade do verde
uint8_t led_b = 20; //Intensidade do azul

bool economia = false;  //Variável para indicar a economia de energia
uint32_t ultimo_tempo_atividade = 0;    //Variável para armazenar o ultimo tempo de atividade
uint volatile numero = 0;      //Variável para inicializar o numero com 0, indicando a camera 0 (WS2812B)
uint16_t adc_x = 0, adc_y = 0;  //Variáveis para armazenar os valores do joystick
bool modo_monitoramento = false;    //Variável para indicar o modo de monitoramento
uint buzzer_slice;  //Slice para o buzzer

/////////////////////////WEBSERVER////////////////////////////////////////////////
//Prototipagem
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void tratar_requisicao_http(char *request);

//WebServer: Início no main()
void iniciar_webserver(){
    if(cyw43_arch_init()) return;
    cyw43_arch_enable_sta_mode();

    printf("Conectando ao Wi-Fi...\n");
    while(cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)){
        printf("Falha ao conectar!\n");
        sleep_ms(3000);
    }
    printf("Conectado! IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));

    struct tcp_pcb *server = tcp_new();
    tcp_bind(server, IP_ADDR_ANY, 80);
    server = tcp_listen(server);
    tcp_accept(server, tcp_server_accept);
}

//Aceita conexão TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err){
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

//Requisição HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err){
    if(!p) return tcp_close(tpcb);

    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';
    tratar_requisicao_http(request);

    // Coleta de dados para status
    int intensidade_percentual = (abs(adc_x - 2048) * 100) / 2048;
    if(intensidade_percentual > 100) intensidade_percentual = 100;
    bool atividade = abs(adc_y - 2048) > 500;

    char html[1024];
    snprintf(html, sizeof(html),
         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
         "<html><head><title>Iluminacao Inteligente</title></head>"
         "<body>"
         "<h1>Sistema de Iluminacao</h1>"
         "<h2>Area atual: %d</h2>"
         "<p>Luminosidade atual: %d%%</p>"
         "<p>Presenca: %s</p>"
         "<br><form action=\"/area_prev\"><button>&larr; Area anterior</button></form>"
         "<form action=\"/area_next\"><button>Proxima area &rarr;</button></form>"
         "<br><form action=\"/aumentar_luz\"><button>+ Aumentar Luz</button></form>"
         "<form action=\"/diminuir_luz\"><button>- Diminuir Luz</button></form>"
         "</body></html>",
         numero,
         intensidade_percentual,
         atividade ? "Sim" : "Nao");

    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    free(request);
    pbuf_free(p);
    return ERR_OK;
}

//Trata a requisição HTTP
void tratar_requisicao_http(char *request){
    if (strstr(request, "GET /area_prev")){
    numero--;   //Incrementa o valor do numero (matriz de leds)
        if(numero == -1){   //Se chegar no 0 e for incrementado, volta ao 9
            numero = 9; //Retorna ao 9
        }
    }else if(strstr(request, "GET /area_next")){
        numero++;   //Incrementa o valor do numero (matriz de leds)
        if(numero == 10){   //Se chegar no 9 e for incrementado, volta ao 0
            numero = 0; //Retorna ao 0
        }
    }else if(strstr(request, "GET /aumentar_luz")){
        if(led_r < 250){
            led_r += 10; led_g += 10; led_b += 10;
        }
    }else if(strstr(request, "GET /diminuir_luz")){
        if(led_r > 10){
            led_r -= 10; led_g -= 10; led_b -= 10;
        }
    }
}

//////////////////////////////////////////BASE PRONTA//////////////////////////////////////////////////////////////////////
//Display SSD1306
ssd1306_t ssd;

//Função para ligar um LED
static inline void put_pixel(uint32_t pixel_grb){
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

//Função para converter cores RGB para um valor de 32 bits
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b){
    return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)(b);
}

//Desenhos dos numeros para a Matriz de leds
bool led_numeros[NUM_NUMBERS][NUM_PIXELS] = {
    //Número 0
    {
    0, 1, 1, 1, 0,      
    0, 1, 0, 1, 0, 
    0, 1, 0, 1, 0,   
    0, 1, 0, 1, 0,  
    0, 1, 1, 1, 0   
    },

    //Número 1
    {0, 1, 1, 1, 0,      
    0, 0, 1, 0, 0, 
    0, 0, 1, 0, 0,    
    0, 1, 1, 0, 0,  
    0, 0, 1, 0, 0   
    },

    //Número 2
    {0, 1, 1, 1, 0,      
    0, 1, 0, 0, 0, 
    0, 1, 1, 1, 0,    
    0, 0, 0, 1, 0,
    0, 1, 1, 1, 0   
    },

    //Número 3
    {0, 1, 1, 1, 0,      
    0, 0, 0, 1, 0, 
    0, 1, 1, 1, 0,    
    0, 0, 0, 1, 0,  
    0, 1, 1, 1, 0   
    },

    //Número 4
    {0, 1, 0, 0, 0,      
    0, 0, 0, 1, 0, 
    0, 1, 1, 1, 0,    
    0, 1, 0, 1, 0,     
    0, 1, 0, 1, 0   
    },

    //Número 5
    {0, 1, 1, 1, 0,      
    0, 0, 0, 1, 0, 
    0, 1, 1, 1, 0,   
    0, 1, 0, 0, 0,  
    0, 1, 1, 1, 0   
    },

    //Número 6
    {0, 1, 1, 1, 0,      
    0, 1, 0, 1, 0, 
    0, 1, 1, 1, 0,    
    0, 1, 0, 0, 0,  
    0, 1, 1, 1, 0   
    },

    //Número 7
    {0, 1, 0, 0, 0,      
    0, 0, 0, 1, 0,   
    0, 1, 0, 0, 0,    
    0, 0, 0, 1, 0,  
    0, 1, 1, 1, 0  
    },

    //Número 8
    {0, 1, 1, 1, 0,      
    0, 1, 0, 1, 0, 
    0, 1, 1, 1, 0,    
    0, 1, 0, 1, 0,  
    0, 1, 1, 1, 0   
    },

    //Número 9
    {0, 1, 1, 1, 0,      
    0, 0, 0, 1, 0, 
    0, 1, 1, 1, 0,    
    0, 1, 0, 1, 0,  
    0, 1, 1, 1, 0   
    },

    //APAGAR OS LEDS, representado pelo número (posição) 10
    {0, 0, 0, 0, 0,      
    0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0,    
    0, 0, 0, 0, 0,  
    0, 0, 0, 0, 0   
    }
};

//Função para envio dos dados para a matriz de leds
void set_one_led(uint8_t r, uint8_t g, uint8_t b, int numero){
    //Define a cor com base nos parâmetros fornecidos
    uint32_t color = urgb_u32(r, g, b);

    //Define todos os LEDs com a cor especificada
    for(int i = 0; i < NUM_PIXELS; i++){
        if(led_numeros[numero][i]){     //Chama a matriz de leds com base no numero passado
            put_pixel(color);           //Liga o LED com um no buffer
        }else{
            put_pixel(0);               //Desliga os LEDs com zero no buffer
        }
    }
}

//Função para modularizar a inicialização do hardware
void inicializar_componentes(){
    stdio_init_all();

    //Inicializa botões
    gpio_init(BOTAO_A);
    gpio_set_dir(BOTAO_A, GPIO_IN);
    gpio_pull_up(BOTAO_A);
    gpio_init(BOTAO_B);
    gpio_set_dir(BOTAO_B, GPIO_IN);
    gpio_pull_up(BOTAO_B);

    // Inicializa LED Vermelho
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_put(LED_RED, 0);

    //Inicializa o pio
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, WS2812_PIN, 800000, IS_RGBW);
    
    //Inicializa ADC para leitura do Joystick
    adc_init();
    adc_gpio_init(JOYSTICK_X);
    adc_gpio_init(JOYSTICK_Y);

    //Inicializa buzzer
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    buzzer_slice = pwm_gpio_to_slice_num(BUZZER_PIN);   //Slice para o buzzer
    float clkdiv = 125.0f; // Clock divisor
    uint16_t wrap = (uint16_t)((125000000 / (clkdiv * 1000)) - 1);      //Valor do Wrap
    pwm_set_clkdiv(buzzer_slice, clkdiv);       //Define o clock
    pwm_set_wrap(buzzer_slice, wrap);           //Define o wrap
    pwm_set_gpio_level(BUZZER_PIN, wrap * 0.3f); //Define duty
    pwm_set_enabled(buzzer_slice, false); //Começa desligado

    //Inicializa I2C para o display SSD1306
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);  //Dados
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);  //Clock
    //Define como resistor de pull-up interno
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    
    // Inicializa display
    ssd1306_init(&ssd, 128, 64, false, 0x3C, i2c1);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);
}

//Função para tocar o buzzer simulando a entrada no modo de economia
void bip_intercalado_suave(){
        pwm_set_enabled(buzzer_slice, true);
        sleep_ms(200);         //Duração do som
        pwm_set_enabled(buzzer_slice, false);
        sleep_ms(800);         //Pausa  
}

//Debounce do botão (evita leituras falsas)
bool debounce_botao(uint gpio){
    static uint32_t ultimo_tempo = 0;   
    uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());   //Tempo atual

    if (gpio_get(gpio) == 0 && (tempo_atual - ultimo_tempo) > 200){ //200ms de debounce
        ultimo_tempo = tempo_atual;
        return true;
    }
    return false;
}

//Função para as chamadas de interrupções nos botões A e B
void gpio_irq_handler(uint gpio, uint32_t events){
    if (gpio == BOTAO_A && debounce_botao(BOTAO_A)){
    numero++;   //Incrementa o valor do numero (matriz de leds)
        if(numero == 10){   //Se chegar no 9 e for incrementado, volta ao 0
            numero = 0; //Retorna ao 0
        }
    set_one_led(led_r, led_g, led_b, numero);
    }
    if(gpio == BOTAO_B && debounce_botao(BOTAO_B)){
        modo_monitoramento = !modo_monitoramento;   //Inverte o estado do monitoramento
    }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//Função para exibir informações no display
void display_info(int luminosidade, bool atividade){
    char buffer[32];    //Buffer para armazenar a string
    ssd1306_fill(&ssd, false);  //Limpa a tela
    
    //Verifica se estamos em modo de economia ou nao para exibir informacoes diferentes
    if(economia){
        sprintf(buffer, "Area %d", numero);
        ssd1306_draw_string(&ssd, buffer, 10, 10);
        sprintf(buffer, "Modo Economia");
        ssd1306_draw_string(&ssd, buffer, 10, 30);
    }else{
        sprintf(buffer, "Area %d", numero);
        ssd1306_draw_string(&ssd, buffer, 10, 10);
        sprintf(buffer, "Luz em %d", luminosidade);
        ssd1306_draw_string(&ssd, buffer, 10, 30);
        sprintf(buffer, "Presenca %s", atividade ? "Sim" : "Nao");
        ssd1306_draw_string(&ssd, buffer, 10, 50);
    }
    ssd1306_send_data(&ssd);    //Envia para o display
}

//Função para atualizar a intensidade da luz dos leds conforme a posição do joystick no eixo X
void atualizar_leds(int eixo_x){
    if(economia) return;    //Ignora se estamos em economia, já que estará apagado
    //Calcula distância do centro (aproximadamente 2048)
    int intensidade = abs(eixo_x - 2048) / 8; //Escala para 0-255 aprox
    if (intensidade > 255) intensidade = 255;   //Limita para 0-255
    set_one_led(intensidade, intensidade, intensidade, numero); //Envia para a matriz de leds a intensidade da luz (branca) da area
}

//Função para verificar se há presença de atividade na area com base no eixo Y
void verificar_presenca(int eixo_y){
    int distancia = abs(eixo_y - 2048); //Calcula distância do centro

    //Ativa a economia de energia
    if(distancia < 500){ //Sem atividade, intervalo do centro
        if (to_ms_since_boot(get_absolute_time()) - ultimo_tempo_atividade > 2000){     //2 segundos sem atividade
            economia = true;    //Ativa a economia
            set_one_led(0, 0, 0, 10); //Apaga a luz da matriz de leds, utilizando o indice 10 definido
            gpio_put(LED_RED, 1); //Liga o LED vermelho, indicando modo de economia
            bip_intercalado_suave();    //Toca o buzzer para indicar a economia
        }
    }else{
        economia = false;   //Desativa a economia
        gpio_put(LED_RED, 0); //atividade detectada
        gpio_put(BUZZER_PIN, 0);    //Desliga o buzzer
        ultimo_tempo_atividade = to_ms_since_boot(get_absolute_time()); //Armazena o tempo da atividade
    }
}

int main(){
    inicializar_componentes(); //Inicializar GPIOs, protocolos, comunicação...
    
    iniciar_webserver();

    //Configura as chamadas de interrupções para os botões A e B
    gpio_set_irq_enabled_with_callback(BOTAO_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    gpio_set_irq_enabled_with_callback(BOTAO_B, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    while(true){
        int intensidade_percentual = 0; //Variável para armazenar a intensidade da luz
        adc_select_input(0);            //Leitura do eixo Y
        int eixo_y = adc_read();
        adc_select_input(1);            //Leitura do eixo X
        int eixo_x = adc_read();
    
        intensidade_percentual = (abs(eixo_x - 2048) * 100) / 2048;     //Calcula a intensidade da luz em percentual
        if(intensidade_percentual > 100) intensidade_percentual = 100;  //Limita para 0-99

        display_info(intensidade_percentual, abs(eixo_y - 2048) > 500); //Mostra as informações no display

        verificar_presenca(eixo_y);     //Sempre verificar presença primeiro
        atualizar_leds(eixo_x);         //Só vai atualizar se economia == false
        if(!economia){
        set_one_led(led_r, led_g, led_b, numero);
        }       
        
        //UART - Comunicação Serial
        if(economia){   //Se estamos em economia
            printf("Area: %d | Modo Economia Ativado\n", numero);
        }else{
        printf("Area: %d | Luz: %d | Presenca: %s\n", numero, intensidade_percentual,
            abs(eixo_y - 2048) > 500 ? "Sim" : "Nao");
        }

        sleep_ms(300);
    }
    return 0;
}