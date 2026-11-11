// SpotiBacan: los datos pasan de /config/NXtify a /config/SpotiBacan (sesion, cachés, ajustes), sin volver a vincular.
// La usan el servicio de fondo (arranca primero, con la consola) y la app: la que llegue antes mueve todo; si la
// carpeta nueva ya existe, se mueve lo que falte, archivo por archivo. La vieja queda vacia (no se borra nada que no
// se haya movido).
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>

inline void spotibacanMigrateConfig(const char* root = "sdmc:") {
    const std::string oldDir = std::string(root) + "/config/NXtify";
    const std::string newDir = std::string(root) + "/config/SpotiBacan";
    struct stat st;
    if (stat(oldDir.c_str(), &st) != 0) return;   // nada que mover
    mkdir((std::string(root) + "/config").c_str(), 0777);
    if (stat(newDir.c_str(), &st) != 0) {
        if (rename(oldDir.c_str(), newDir.c_str()) == 0) return;
        mkdir(newDir.c_str(), 0777);
    }
    if (DIR* d = opendir(oldDir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string to = newDir + "/" + n;
            if (stat(to.c_str(), &st) == 0) continue;   // ya hay uno nuevo: se queda el nuevo
            rename((oldDir + "/" + n).c_str(), to.c_str());
        }
        closedir(d);
    }
}
