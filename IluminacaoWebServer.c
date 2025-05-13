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
#include "inc/matriz_leds.h"

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#define WIFI_SSID "PRF"
#define WIFI_PASSWORD "@hfs0800"

//Definição de GPIOs
#define JOYSTICK_Y 27  //ADC1
#define WS2812_PIN 7    //Pino do WS2812
#define LED_RED 13      //Pino do LED vermelho
#define BUZZER_PIN 21   //Pino do buzzer
#define I2C_SDA 14      //Pino SDA - Dados
#define I2C_SCL 15      //Pino SCL - Clock
#define IS_RGBW false   //Maquina PIO para RGBW

#define NUM_AREAS 10
typedef struct{
    int luminosidade;  //0-100%
    //posso guardar tambem se ha presenca ou nao aqui, mas deixei de fora na simulação por enquanto
}AreaStatus;

AreaStatus areas[NUM_AREAS];  //Vetor com dados de cada área

//Variável global para armazenar a cor (Entre 0 e 255 para intensidade)
uint8_t led_r = 20; //Intensidade do vermelho
uint8_t led_g = 20; //Intensidade do verde
uint8_t led_b = 20; //Intensidade do azul

bool economia = false;  //Variável para indicar a economia de energia
uint32_t ultimo_tempo_atividade = 0;    //Variável para armazenar o ultimo tempo de atividade
uint volatile numero = 0;      //Variável para inicializar o numero com 0, indicando a camera 0 (WS2812B)
uint16_t adc_y = 0;  //Variáveis para armazenar os valores do joystick
volatile bool alarme_disparado = false;    //Variável para indicar o modo de monitoramento
uint buzzer_slice;  //Slice para o buzzer

//Prototipagem
void set_one_led(uint8_t r, uint8_t g, uint8_t b, int numero);
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void tratar_requisicao_http(char *request);

//////////////////////////////////////////BASE PRONTA//////////////////////////////////////////////////////////////////////
//Display SSD1306
ssd1306_t ssd;

//Função para modularizar a inicialização do hardware
void inicializar_componentes(){
    stdio_init_all();
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

//Função para verificar se há presença de atividade na area com base no eixo Y
void verificar_presenca(int eixo_y){
    int distancia = abs(eixo_y - 2048); //Calcula distância do centro

    //Ativa a economia de energia
    if(distancia < 500){ //Sem atividade, intervalo do centro
        if (to_ms_since_boot(get_absolute_time()) - ultimo_tempo_atividade > 2000){     //2 segundos sem atividade
            economia = true;    //Ativa a economia
            set_one_led(0, 0, 0, 10); //Apaga a luz da matriz de leds, utilizando o indice 10 definido
            gpio_put(LED_RED, 1); //Liga o LED vermelho, indicando modo de economia
        }
    }else{
        economia = false;   //Desativa a economia
        gpio_put(LED_RED, 0); //atividade detectada
        ultimo_tempo_atividade = to_ms_since_boot(get_absolute_time()); //Armazena o tempo da atividade
    }
}

/////////////////////////WEBSERVER////////////////////////////////////////////////
// Atualiza LEDs conforme intensidade definida via Web
void atualizar_leds_web(){
    if(economia) return;    //Nao atualiza se estamos em modo de economia
    int intensidade = (areas[numero].luminosidade * 255) / 100; //Calcula intensidade
    set_one_led(intensidade, intensidade, intensidade, numero); //Atualiza leds na matriz de LEDs
}

//WebServer: Início no main()
void iniciar_webserver(){
    if(cyw43_arch_init()) return;   //Inicia o Wi-Fi
    cyw43_arch_enable_sta_mode();

    printf("Conectando ao Wi-Fi...\n"); 
    while(cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)){    //Conecta ao Wi-Fi - loop
        printf("Falha ao conectar!\n");
        sleep_ms(3000);
    }
    printf("Conectado! IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));    //Conectado, e exibe o IP da rede no serial monitor

    struct tcp_pcb *server = tcp_new(); //Cria o servidor
    tcp_bind(server, IP_ADDR_ANY, 80);  //Binda na porta 80
    server = tcp_listen(server);        //Inicia o servidor
    tcp_accept(server, tcp_server_accept);  //Aceita conexoes
}

//Aceita conexão TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err){
    tcp_recv(newpcb, tcp_server_recv);  //Recebe dados da conexao
    return ERR_OK;
}

//Requisição HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err){
    if(!p) return tcp_close(tpcb);  //Se nao houver dados, fecha a conexao

    char *request = (char *)malloc(p->len + 1); //Aloca memória para o request
    memcpy(request, p->payload, p->len);    //Copia o request
    request[p->len] = '\0'; //Terminador de string
    tratar_requisicao_http(request);    //Trata a requisição HTTP, chama a funcao

    bool atividade = abs(adc_y - 2048) > 500;   //Verifica se houve atividade

    char html[1024];    //Aloca memória para o HTML
snprintf(html, sizeof(html),    //Cria o HTML
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'><title>Iluminaçãoo</title>"
    "<style>"
    "body {background:#46dd73;font-family:sans-serif;text-align:center;margin-top:30px;}"
    "h1,h2,h3,h4 {margin:10px;}"
    ".btns {display:flex;justify-content:center;gap:10px;flex-wrap:wrap;margin-top: 20px;}"
    "button {background:lightgray;font-size:20px;padding:10px 20px;border-radius:8px;border:none;}"
    "</style></head><body>"

    "<h1>Sistema de Iluminacao</h1>"

    "<div class='btns'>"
    "<form action='/area_prev'><button>&larr; Anterior</button></form>"
    "<form action='/area_next'><button>Proxima &rarr;</button></form>"
    "</div>"

    "<h2>Area: %d</h2>"
    "<h3>Luz: %d%%</h3>"
    "<h3>Presenca: %s</h3>"

    "<div class='btns'>"
    "<form action='/diminuir_luz'><button>- Luz</button></form>"
    "<form action='/aumentar_luz'><button>+ Luz</button></form>"
    "</div>"

    "<div class='btns'>"
    "<form action='/alarme_on'><button>Dispara Alarme</button></form>"
    "<form action='/alarme_off'><button>Para Alarme</button></form>"
    "</div>"

    "<div class='btns'>"
    "<form action='/refresh'><button>Atualizar</button></form>"
    "</div>"

    "</body></html>",
    numero,
    areas[numero].luminosidade,
    atividade ? "Sim" : "Nao"
);

    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);   //Envia o HTML
    tcp_output(tpcb);                   //Envia os dados
    free(request);                      //Libera a memória
    pbuf_free(p);
    return ERR_OK;                  
}

//Trata a requisição HTTP
void tratar_requisicao_http(char *request){
    if (strstr(request, "GET /area_prev")){ //Verifica se é o request "area_prev"
    numero--;   //Incrementa o valor do numero (matriz de leds)
        if(numero == -1){   //Se chegar no 0 e for incrementado, volta ao 9
            numero = 9; //Retorna ao 9
        }
    }else if(strstr(request, "GET /area_next")){    //Verifica se é o request "area_next"
        numero++;   //Incrementa o valor do numero (matriz de leds)
        if(numero == 10){   //Se chegar no 9 e for incrementado, volta ao 0
            numero = 0; //Retorna ao 0
        }
    }else if(strstr(request, "GET /aumentar_luz")){     //Verifica se é o request "aumentar_luz"
        areas[numero].luminosidade += 10;     //Aumenta a luminosidade em 10%
        if(areas[numero].luminosidade > 100) areas[numero].luminosidade = 100;
    }else if(strstr(request, "GET /diminuir_luz")){ //Verifica se é o request "diminuir_luz"
        areas[numero].luminosidade= - 10;   //Diminui a luminosidade em 10%
        if(areas[numero].luminosidade < 0) areas[numero].luminosidade = 0;
        //set_one_led(led_r, led_g, led_b, numero);
    }else if(strstr(request, "GET /alarme_on")){    //Verifica se é o request "alarme_on"
        alarme_disparado = true;            //Envia para a variavel global como true, ligado
    }
    else if(strstr(request, "GET /alarme_off")){    //Verifica se é o request "alarme_off"
        alarme_disparado = false;               //Envia para a variavel global como false, desligado
        pwm_set_enabled(buzzer_slice, false); //Garantir desligamento imediato
    }
    else if(strstr(request, "GET /refresh")){       //Verifica se é o request "refresh"
    //Nada, so manda a requisição para atualizar com os dados atualizados
    }
}

int main(){
    inicializar_componentes();  //Inicia os componentes
    iniciar_webserver();        //Inicia o webserver

    while(true){
        //Leitura do eixo Y, para leitura de presença
        adc_select_input(0);
        int eixo_y = adc_read();
        adc_y = eixo_y; // Armazena global para acesso em Web

        //Limita luminosidade entre 0 e 100
        if (areas[numero].luminosidade > 100) areas[numero].luminosidade = 100;
        if (areas[numero].luminosidade < 0) areas[numero].luminosidade = 0; 

        display_info(areas[numero].luminosidade, abs(eixo_y - 2048) > 500); //Exibe informacoes no display
        verificar_presenca(eixo_y); //Verifica se houve atividade na area
        atualizar_leds_web();   //Atualiza leds conforme intensidade definida via Web

        //Alarme sonoro intermitente controlado por botão web
        if(alarme_disparado){
            pwm_set_enabled(buzzer_slice, true);
            sleep_ms(200);
            pwm_set_enabled(buzzer_slice, false);
            sleep_ms(800);
        }else{
            sleep_ms(300);
        }

        printf("Area: %d | Luz: %d | Presenca: %s\n", numero, areas[numero].luminosidade,
               abs(eixo_y - 2048) > 500 ? "Sim" : "Nao");
    }
    return 0;
}