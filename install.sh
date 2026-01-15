#!/bin/bash

# Kolory dla outputu
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Funkcja do wyświetlania kolorowych komunikatów
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Sprawdź czy skrypt jest uruchomiony z uprawnieniami root
if [[ $EUID -ne 0 ]]; then
   print_error "Ten skrypt musi być uruchomiony z uprawnieniami root (sudo)"
   exit 1
fi

# Funkcja sprawdzająca czy pakiet jest zainstalowany
check_package() {
    dpkg -s "$1" &>/dev/null
    return $?
}

# Funkcja instalująca pakiety
install_packages() {
    local packages=("$@")
    local missing_packages=()
    
    for package in "${packages[@]}"; do
        if ! check_package "$package"; then
            print_warning "Brakuje pakietu: $package"
            missing_packages+=("$package")
        else
            print_info "Pakiet $package jest zainstalowany"
        fi
    done
    
    if [ ${#missing_packages[@]} -gt 0 ]; then
        print_info "Instaluję brakujące pakiety: ${missing_packages[*]}"
        apt-get update
        apt-get install -y "${missing_packages[@]}"
        
        if [ $? -ne 0 ]; then
            print_error "Błąd podczas instalacji pakietów"
            return 1
        fi
        print_info "Pakiety zainstalowane pomyślnie"
    else
        print_info "Wszystkie wymagane pakiety są zainstalowane"
    fi
    
    return 0
}

# Funkcja zatrzymująca serwis
stop_service() {
    local service_name=$1
    
    if systemctl is-active --quiet "$service_name"; then
        print_info "Zatrzymuję serwis $service_name..."
        systemctl stop "$service_name"
        if [ $? -eq 0 ]; then
            print_info "Serwis $service_name zatrzymany"
        else
            print_error "Nie udało się zatrzymać serwisu $service_name"
            return 1
        fi
    else
        print_info "Serwis $service_name nie jest aktywny"
    fi
    
    return 0
}

# Funkcja kompilująca serwis
compile_service() {
    local service_dir=$1
    local service_name=$2
    
    print_info "Kompilacja serwisu $service_name..."
    
    cd "$service_dir" || {
        print_error "Nie można wejść do katalogu $service_dir"
        return 1
    }
    
    make clean
    make
    
    if [ $? -ne 0 ]; then
        print_error "Kompilacja $service_name nie powiodła się"
        cd - > /dev/null
        return 1
    fi
    
    print_info "Kompilacja $service_name zakończona pomyślnie"
    cd - > /dev/null
    return 0
}

# Funkcja instalująca serwis
install_service() {
    local service_dir=$1
    local service_name=$2
    local service_file=$3
    local binary_name="${service_name}_service"
    
    # Kopiowanie binarki
    print_info "Kopiuję binarkę $binary_name do /usr/local/bin/..."
    cp "$service_dir/$binary_name" /usr/local/bin/
    
    if [ $? -ne 0 ]; then
        print_error "Nie udało się skopiować binarki $binary_name"
        return 1
    fi
    
    chmod +x "/usr/local/bin/$binary_name"
    print_info "Binarka $binary_name zainstalowana"
    
    # Kopiowanie pliku serwisowego
    print_info "Kopiuję plik serwisowy $service_file do /etc/systemd/system/..."
    cp "service_files/$service_file" /etc/systemd/system/
    
    if [ $? -ne 0 ]; then
        print_error "Nie udało się skopiować pliku serwisowego $service_file"
        return 1
    fi
    
    print_info "Plik serwisowy $service_file zainstalowany"
    
    return 0
}

# Główna część skryptu
print_info "=== Instalacja Raspberry Services ==="
echo ""

# Krok 1: Zatrzymanie wszystkich serwisów
print_info "Krok 1: Sprawdzanie i zatrzymywanie aktywnych serwisów..."
if [ -f "/etc/systemd/system/pirService.service" ]; then
    stop_service "pirService.service"
fi
if [ -f "/etc/systemd/system/camService.service" ]; then
    stop_service "camService.service"
fi
if [ -f "/etc/systemd/system/cardService.service" ]; then
    stop_service "cardService.service"
fi
echo ""

# Krok 2: Instalacja PIR Service
print_info "Krok 2: Instalacja PIR Service..."
print_info "Sprawdzam wymagane pakiety dla PIR Service..."
install_packages libwebsockets-dev libgpiod-dev gcc make

if [ $? -eq 0 ]; then
    compile_service "pir_service" "pir"
    if [ $? -eq 0 ]; then
        install_service "pir_service" "pir" "pirService.service"
        print_info "PIR Service zainstalowany pomyślnie"
    fi
fi
echo ""

# Krok 3: Instalacja CAM Service
print_info "Krok 3: Instalacja CAM Service..."
print_info "Sprawdzam wymagane pakiety dla CAM Service..."
install_packages libwebsockets-dev libopencv-dev g++ gcc make pkg-config

if [ $? -eq 0 ]; then
    compile_service "cam_service" "cam"
    if [ $? -eq 0 ]; then
        install_service "cam_service" "cam" "camService.service"
        print_info "CAM Service zainstalowany pomyślnie"
    fi
fi
echo ""

# Krok 4: Instalacja CARD Service
print_info "Krok 4: Instalacja CARD Service..."
print_info "Sprawdzam wymagane pakiety dla CARD Service..."
install_packages libwebsockets-dev gcc make

if [ $? -eq 0 ]; then
    compile_service "card_service" "card"
    if [ $? -eq 0 ]; then
        install_service "card_service" "card" "cardService.service"
        print_info "CARD Service zainstalowany pomyślnie"
    fi
fi
echo ""

# Przeładowanie systemd i uruchomienie serwisów
print_info "=== Przeładowanie systemd i uruchamianie serwisów ==="
systemctl daemon-reload

# Włączenie i uruchomienie serwisów
print_info "Włączam i uruchamiam serwisy..."

# PIR Service
systemctl enable pirService.service 2>/dev/null
systemctl start pirService.service
sleep 1
if systemctl is-active --quiet pirService.service; then
    print_info "pirService.service uruchomiony"
else
    print_error "pirService.service nie uruchomił się"
fi

# CAM Service
systemctl enable camService.service 2>/dev/null
systemctl start camService.service
sleep 1
if systemctl is-active --quiet camService.service; then
    print_info "camService.service uruchomiony"
else
    print_error "camService.service nie uruchomił się"
fi

# CARD Service
systemctl enable cardService.service 2>/dev/null
systemctl start cardService.service
sleep 1
if systemctl is-active --quiet cardService.service; then
    print_info "cardService.service uruchomiony"
else
    print_error "cardService.service nie uruchomił się"
fi

echo ""
print_info "=== Instalacja zakończona ==="
echo ""
print_info "Aby sprawdzić status serwisów, użyj:"
echo "  sudo systemctl status pirService.service"
echo "  sudo systemctl status camService.service"
echo "  sudo systemctl status cardService.service"
echo ""
print_info "Aby zobaczyć logi serwisów, użyj:"
echo "  sudo journalctl -u pirService.service -f"
echo "  sudo journalctl -u camService.service -f"
echo "  sudo journalctl -u cardService.service -f"