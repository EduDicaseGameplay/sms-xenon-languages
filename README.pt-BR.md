# Super Mario Sunshine no Xbox 360, em português e outros idiomas

[English](README.md) | **Português (Brasil)**

Este repositório é o **port de Super Mario Sunshine para Xbox 360** (feito por outras pessoas, veja os
[créditos](#créditos)) com uma adição minha: poder jogar em **português do Brasil, espanhol, francês,
alemão e italiano**, usando as suas próprias ISOs.

> **Eu não criei o port.** Todo o trabalho de descompilação, o port para PC e o port para Xbox 360
> são dos autores listados nos [créditos](#créditos). O que este repositório acrescenta é o suporte
> para gerar os idiomas a partir da ISO europeia e da tradução PT-BR, uma correção no build e este
> passo a passo. A lista exata de arquivos que eu criei ou alterei está em [ALTERACOES.md](ALTERACOES.md).

> **Nenhum dado do jogo está aqui.** Super Mario Sunshine pertence à Nintendo. Você precisa das suas
> próprias cópias do jogo. ISOs, arquivos extraídos e pacotes gerados **nunca** devem ser enviados
> para o repositório (o [.gitignore](.gitignore) já bloqueia).

---

## Como funciona

O port roda só com a **ISO americana** (GMSE01), que é a base do jogo. Os outros idiomas vêm de outras ISOs:

| ISO | Para que serve |
|---|---|
| **Super Mario Sunshine (USA)** — GMSE01 | Base do jogo (obrigatória) |
| **Super Mario Sunshine (Europe)** — GMSP01 | Gera os pacotes `es`, `fr`, `de` e `it` |
| **Tradução PT-BR** (feita sobre a europeia, também GMSP01) | Gera o pacote `pt` |

O script [`sms_language.py`](sms-xenon-source/xenon/dist/sms_language.py) lê a ISO americana e a europeia
(ou a BR) e cria a pasta `mods/languages/<código>`. O port já sabe carregar essa pasta. Quando o jogo
inicia, um **menu de idioma** (a caixa de mensagem do próprio Xbox) lista o inglês e todos os pacotes
instalados, e você escolhe com o controle, sem editar nenhum arquivo.

---

## Estrutura de pastas usada neste guia

```
Ubuntu (WSL):
~/sms-xenon-languages/            <- este repositório (uma cópia DENTRO do Ubuntu)
├── sms-xenon-source/             <- código do port
└── build-xenon/stage/            <- criado pelo build

Windows:
C:\SMS\isos\                      <- coloque aqui as suas ISOs
C:\SMS\jogo-xbox\                 <- criado no passo 14: a pasta que vai para o Xbox
C:\work\sms-xenon\stage\          <- criado no passo 9: onde o Visual Studio compila
```

> O build sempre roda a partir de uma cópia **dentro do Ubuntu** (`~/...`), nunca de `/mnt/c`. Você
> pode pegar o código clonando no Ubuntu, ou baixando o ZIP / clonando no Windows e copiando a pasta
> para o Ubuntu (passo 6). Os dois jeitos foram testados e dão os mesmos arquivos.

## O que você precisa

- Windows 10/11 com **Visual Studio 2010** e o **Xbox 360 SDK (XDK)** instalados (não são distribuídos aqui).
- Um Xbox 360 desbloqueado (que rode homebrew), com HD ou pendrive.
- As suas ISOs: a **americana** (obrigatória), mais a **europeia** e/ou a **PT-BR**.

> **Só quer jogar, sem compilar?** O `default.xex` do pacote oficial do port (sms-xenon-f233c15) também
> lê os pacotes de idioma, mas **não tem o menu de idioma**: com ele, coloque `SMS_LANGUAGE=pt` (ou `es`,
> `fr`, `de`, `it`) no `sms_env.txt`. Pule as partes 1 a 3 e vá direto para a [parte 4](#parte-4-montar-a-pasta-do-jogo-com-os-idiomas-no-ubuntu).

---

## Parte 1: Instalar o Ubuntu no Windows (uma vez só)

**1.** Abra o **PowerShell como Administrador** (Iniciar → "PowerShell" → botão direito → *Executar como administrador*):
```powershell
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

**2.** Reinicie o computador.

**3.** No PowerShell como Administrador:
```powershell
wsl --install -d Ubuntu-22.04
```
> Se der erro de **virtualização**: ative *Intel VT-x* (Intel) ou *SVM* (AMD) na BIOS e repita, **ou**
> rode `wsl --set-default-version 1` e repita (usa o WSL1, sem mexer na BIOS).

**4.** Abra o **Ubuntu** no menu Iniciar e crie usuário e senha.

---

## Parte 2: Preparar o código (no Ubuntu)

**5.** Instale as ferramentas (uma vez só). As **duas** linhas de `apt install` são obrigatórias:
```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install -y git cmake make python3 patch rsync binutils gcc-multilib g++-multilib libsdl2-dev libegl-dev libgl-dev
sudo apt install -y libsdl2-2.0-0:i386 libgl1:i386 libegl1:i386 libgl1-mesa-dri:i386 libegl-mesa0:i386
```

**6.** Coloque o código dentro do Ubuntu. Escolha **um** jeito:

- **Clonar no Ubuntu:**
  ```bash
  git clone https://github.com/EduDicaseGameplay/sms-xenon-languages.git ~/sms-xenon-languages
  ```
- **Ou baixar no Windows** (no GitHub, *Code → Download ZIP*; extraia, por exemplo, em
  `C:\SMS\sms-xenon-languages-main`) e copiar para o Ubuntu:
  ```bash
  cp -r /mnt/c/SMS/sms-xenon-languages-main ~/sms-xenon-languages
  ```

Depois:
```bash
cd ~/sms-xenon-languages/sms-xenon-source
```

**7.** Aplique os patches e compile a versão Linux. A compilação Linux é **obrigatória**: o build do Xbox lê os objetos dela.
```bash
cmake -S . -B build/linux-32 -DSMS_ARCH=32 -DSMS_GX_BUILD_TESTS=OFF -DSMS_BUNDLE_DISC=
cmake --build build/linux-32 --target sms --parallel $(nproc)
```
O primeiro comando tem que terminar com `-- Generating done` e o segundo com `[100%] Built target sms`.

**8.** Monte o *stage* do Xbox e o projeto do Visual Studio:
```bash
rm -rf ../build-xenon/stage
xenon/tools/xstage --no-sync
python3 xenon/tools/xproj.py . ../build-xenon/stage
```
O `xstage` tem que terminar sem `Traceback`. Uma das últimas linhas é `xstubs: N stubs dropped`.

**9.** Mande o stage para o Windows:
```bash
mkdir -p /mnt/c/work/sms-xenon
rsync -rlt --delete --exclude=/xenon/out/ ~/sms-xenon-languages/build-xenon/stage/ /mnt/c/work/sms-xenon/stage/
```

---

## Parte 3: Compilar (no PowerShell do Windows)

**10.** Abra um **PowerShell normal** (não o Ubuntu; o prompt começa com `PS C:\`):
```powershell
& C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe C:\work\sms-xenon\stage\xenon\sms.vcxproj /m /t:Rebuild /p:Configuration=Master "/p:Platform=Xbox 360"
```
Tem que terminar com **Build succeeded** (os avisos `C4355` são normais). O resultado sai em:
`C:\work\sms-xenon\stage\xenon\out\Master\default.xex`

---

## Parte 4: Montar a pasta do jogo com os idiomas (no Ubuntu)

Coloque as suas ISOs em `C:\SMS\isos\` (no Ubuntu, essa pasta é `/mnt/c/SMS/isos/`).

**11.** Entre na pasta do código:
```bash
cd ~/sms-xenon-languages/sms-xenon-source
mkdir -p /mnt/c/SMS/jogo-xbox
```

**12.** Base do jogo, a partir da ISO americana. Escolha **uma** das opções:

- **Opção A (mais simples):** copie a ISO americana com o nome `GMSE01.iso`:
  ```bash
  cp "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" /mnt/c/SMS/jogo-xbox/GMSE01.iso
  ```
- **Opção B:** extraia os arquivos (cria `files/` e `sys/`):
  ```bash
  python3 xenon/dist/sms_extract.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" /mnt/c/SMS/jogo-xbox
  ```

> Nunca renomeie a ISO europeia ou a BR para `GMSE01.iso`: o código do port é o da versão americana.

**13.** Gere os pacotes de idioma:

Português (da ISO PT-BR), cerca de 1 minuto:
```bash
python3 xenon/dist/sms_language.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" "/mnt/c/SMS/isos/Mario Sunshine (BR).iso" /mnt/c/SMS/jogo-xbox
```
Espanhol, francês, alemão e italiano (da ISO europeia), cerca de 3 minutos:
```bash
python3 xenon/dist/sms_language.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" "/mnt/c/SMS/isos/Super Mario Sunshine (Europe).iso" /mnt/c/SMS/jogo-xbox
```
(Troque os nomes das ISOs pelos dos seus arquivos.)

**14.** Copie o executável e o arquivo de opções:
```bash
cp /mnt/c/work/sms-xenon/stage/xenon/out/Master/default.xex /mnt/c/SMS/jogo-xbox/
cp xenon/dist/sms_env.txt /mnt/c/SMS/jogo-xbox/
```
Deixe a linha `SMS_LANGUAGE` do `sms_env.txt` comentada (`# SMS_LANGUAGE=pt`): assim o jogo mostra o menu
de idioma toda vez que inicia. Se quiser sempre o mesmo idioma, tire o `#` e coloque o código (`pt`, `es`,
`fr`, `de` ou `it`): o jogo entra direto nele, sem perguntar.

Como fica a pasta:
```
C:\SMS\jogo-xbox\
├── default.xex
├── sms_env.txt
├── GMSE01.iso           (opção A)  ou  files\ + sys\  (opção B)
└── mods\languages\      (pt  es  fr  de  it)
```

---

## Parte 5: Jogar no Xbox

**15.** Copie **todo o conteúdo** de `C:\SMS\jogo-xbox` para o console (FTP pelo Aurora/FSD ou pendrive),
por exemplo para `Hdd1:\Games\Sunshine\`.

**16.** Inicie o `default.xex` pelo Aurora. Depois de uns dois segundos de tela preta, aparece o menu de idioma:

```
Super Mario Sunshine
Escolha o idioma / Choose your language / Elige el idioma

  [ English ]  [ Português (Brasil) ]  [ Mais / More / Más > ]
```

A caixa de mensagem do Xbox aceita no máximo três botões, então os idiomas aparecem dois por página, com
**Mais** para os próximos. O **B** volta para a primeira página; na primeira página ele fecha o menu e o
jogo entra em inglês.

Se o jogo abrir em inglês sem você ter escolhido, abra o `sms.log` na pasta do jogo:
- linhas `language menu: ...` → o que o menu fez (o botão escolhido, ou por que fechou);
- `no pack in mods/languages/pt` → a pasta `mods` não está ao lado do `default.xex`;
- `language pt: ... replaced` → o pacote carregou.

---

## Limitações conhecidas

- **Algumas telas ou textos podem continuar em inglês.** Os pacotes cobrem diálogos, menus, guia,
  seleção de fase, tela de save, legendas e título, mas nem tudo o que a versão europeia muda foi mapeado.
- **A ISO americana é sempre necessária**: a europeia e a BR fornecem só os textos.
- As legendas dos vídeos vêm do disco PAL e podem ficar um pouco fora de sincronia.

Encontrou um texto que não traduziu? Abra uma *issue* dizendo em qual tela.

---

## Créditos

| Quem | O quê | Licença |
|---|---|---|
| **Nintendo** | Super Mario Sunshine (o jogo e todos os dados do disco) | — (não incluídos) |
| **18woldemar** | Port para Xbox 360: renderizador para a GPU do 360, camada de plataforma, áudio, controle e ferramentas de build (`xenon/`) | MIT |
| **SoftwareAura LLC / [chasem-dev](https://github.com/chasem-dev)** | [Super Mario Sunshine PC port](https://github.com/chasem-dev/sms-pc-port), base do port de Xbox, e o [mod SDK](https://github.com/chasem-dev/sms-mod-sdk) | MIT |
| **[chasem-dev/sms-english](https://github.com/chasem-dev/sms-english) e colaboradores** | Descompilação de Super Mario Sunshine (`decomp/`) | CC0 1.0 |
| **Autores da tradução PT-BR** | A tradução para português usada como fonte do pacote `pt` (não incluída) | — |
| **EduDicaseGameplay** (este repositório) | `sms_language.py`, correção do `xstubs.py`, documentação dos idiomas e este guia. Veja [ALTERACOES.md](ALTERACOES.md) | MIT |

O código original veio do pacote `sms-xenon-source-f233c15` (sms-pc-port, branch `xenon`, commit
`f233c150b8407a56c88de794887c3d9726f9ccef`). O primeiro commit deste repositório é esse código **sem
nenhuma alteração**. As minhas mudanças estão nos commits seguintes, então dá para ver exatamente o que
mudou com:
```bash
git diff 26a14ce HEAD -- sms-xenon-source
```
Os avisos de licença originais continuam em [sms-xenon-source/LICENSE](sms-xenon-source/LICENSE),
[sms-xenon-source/xenon/dist/LICENSE.txt](sms-xenon-source/xenon/dist/LICENSE.txt),
[sms-xenon-source/decomp/LICENSE](sms-xenon-source/decomp/LICENSE) e
[sms-xenon-source/sdk/LICENSE](sms-xenon-source/sdk/LICENSE).
