package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io/ioutil"
	"net"
	"net/http"
	"net/smtp"
	"strings"
	"time"
)

func report(w http.ResponseWriter, req *http.Request) {

	body, err := ioutil.ReadAll(req.Body)
	if err != nil {
		fmt.Printf("could not read body: %s\n", err)
	}

	fmt.Println(body)
	fmt.Fprintf(w, "success\n")
}

func headers(w http.ResponseWriter, req *http.Request) {

	for name, headers := range req.Header {
		for _, h := range headers {
			fmt.Fprintf(w, "%v: %v\n", name, h)
		}
	}
}

type timeHandler struct {
	format string
}

func (th timeHandler) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	tm := time.Now().Format(th.format)
	body, err := ioutil.ReadAll(req.Body)
	if err != nil {
		fmt.Printf("could not read body: %s\n", err)
	}

	fmt.Println(body)
	w.Write([]byte("The time is: " + tm))
}

// func main() {
// 	mux := http.NewServeMux()

// 	// Initialise the timeHandler in exactly the same way we would any normal
// 	// struct.
// 	th := timeHandler{format: time.RFC1123}

// 	// Like the previous example, we use the mux.Handle() fnction to register
// 	// this with our ServeMux.
// 	mux.Handle("/report", th)

// 	log.Print("Listening...")
// 	log.Fatal(http.ListenAndServe(":7000", mux))
// }

type AccountData struct {
	Account             string
	Product             string
	Overlimit           int
	OverlimitPercentage string
	BreachCount         int
}

func getOverLimit(w http.ResponseWriter, req *http.Request) {

	fmt.Println(req.URL.RequestURI())
	fmt.Println(req.Host)

	s := strings.TrimPrefix("https://edge-admin.us-east-1.freshedge.net/nelreports/freshlink", "www.")
	host := strings.Split(s, ".")
	region := host[1]
	fmt.Println("region: ", len(host), region)
	// url, err := url.Parse(req.URL.Host)
	// if err == nil {
	// 	hostname := strings.TrimPrefix(url.Hostname(), "www.")
	// 	fmt.Println(hostname)
	// }
	acc := &AccountData{
		Account:             "freshdesk.com",
		Product:             "freshdesk",
		Overlimit:           540000,
		OverlimitPercentage: "30%",
		BreachCount:         15,
	}

	data, _ := json.Marshal(acc)
	w.Header().Set("Access-Control-Allow-Headers", "*")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Access-Control-Allow-Methods", "*")
	w.Write(data)

}

type NELReport struct {
	Age       uint32
	Body      NELReportBody
	Type      string
	Url       string
	UserAgent string `json:"user_agent"`
}

type NELReportBody struct {
	ElapsedTime      uint32 `json:"elapsed_time"`
	Method           string
	Phase            string
	Protocol         string
	Referrer         string
	SamplingFraction float32 `json:"sampling_fraction"`
	ServerIp         string  `json:"server_ip"`
	StatusCode       uint16  `json:"status_code"`
	Type             string
}

func HandleNEL(w http.ResponseWriter, r *http.Request) {

	fmt.Println("received request")
	var nelReports []NELReport

	err := json.NewDecoder(r.Body).Decode(&nelReports)

	if err != nil {
		fmt.Printf("could not read body: %s\n", err)
	}

	for _, report := range nelReports {
		fmt.Println(report)
	}

}

func Home(w http.ResponseWriter, req *http.Request) {
	fmt.Println("received request")
	// w.Header().Set("Access-Control-Allow-Headers", "*")
	// w.Header().Set("Access-Control-Allow-Origin", "*")
	// w.Header().Set("Access-Control-Allow-Methods", "*")
	w.WriteHeader(http.StatusOK)
}

// func main() {

// 	router := mux.NewRouter()
// 	//http.HandleFunc("/report", report)
// 	//http.HandleFunc("/getoverlimitcount/{product}/{account}/{timeRange}", getOverLimit)
// 	fmt.Println("starting server")
// 	router.HandleFunc("/nelreport", HandleNEL).Methods("GET", "POST", "OPTIONS")
// 	router.HandleFunc("/", Home).Methods("GET", "POST", "OPTIONS")
// 	// router.HandleFunc("/getoverlimitcount", getOverLimit).Methods("GET", "POST", "OPTIONS")
// 	log.Fatal(http.ListenAndServe(":4000", router))
// 	//http.ListenAndServe(":4000", nil)
// 	// err := http.ListenAndServeTLS(":7000", "localhost.crt", "localhost.key", nil)
// 	// if err != nil {
// 	// 	log.Fatal("ListenAndServe: ", err)
// 	// }
// }

func main() {
	var d net.Dialer
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	conn, err := d.DialContext(ctx, "tcp", "localhost:10000")

	defer conn.Close()

	client, err := smtp.NewClient(conn, "localhost")

	// EHLO
	err = client.Hello("localhost")

	// //STARTTLS
	err = client.StartTLS(&tls.Config{
		InsecureSkipVerify: true,
	})

	//TLS config
	// tlsconfig := &tls.Config{
	// 	ServerName: "myfreshworks.com",
	// }


	// err = client.StartTLS(tlsconfig)
	// if err != nil {
	// 	fmt.Println(err)
	// }

	cs, ok := client.TLSConnectionState()
	if !ok {
		fmt.Println("TLSConnectionState returned ok == false; want true")
		return
	}
	if cs.Version == 0 || !cs.HandshakeComplete {
		fmt.Println("ConnectionState = expect non-zero Version and HandshakeComplete", cs)
	}

	// AUTH
	err = client.Auth(smtp.PlainAuth("", "username", "password", "localhost"))
	// err = client.Auth(sasl.NewPlainClient("", "username", "password"))

	// MAILFROM
	err = client.Mail("edge@freshworks.com")

	// RCPTTO
	err = client.Rcpt("api-gateway@freshworks.com")

	// DATA
	w, err := client.Data()

	n, err := w.Write([]byte("Hello World\r\n.\r\n"))
	if err != nil {
		fmt.Println("wrote ", n, "bytes")
	}

	err = w.Close()
	if err != nil {
		fmt.Print(err)
	}
	err = client.Quit()
	if err != nil {
		fmt.Print(err)
	}
}
