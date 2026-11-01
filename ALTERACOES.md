# O que foi criado ou alterado

[English](CHANGES.md) | **Português (Brasil)**

Ponto de partida: o código original do port para Xbox 360, `sms-xenon-source-f233c15`, importado
**sem nenhuma alteração** no primeiro commit. Tudo o que mudou depois está listado aqui.

Para ver as diferenças linha a linha:
```bash
git diff HEAD~2 HEAD -- sms-xenon-source
```

## Arquivos do código (`sms-xenon-source/`)

| Arquivo | Tipo | O que mudou |
|---|---|---|
| [`xenon/dist/sms_language.py`](sms-xenon-source/xenon/dist/sms_language.py) | **criado** | Gera os pacotes de idioma (`mods/languages/<código>`) a partir da ISO americana + uma ISO PAL (europeia ou PT-BR). |
| [`xenon/dist/sms_extract.py`](sms-xenon-source/xenon/dist/sms_extract.py) | alterado (+4 linhas) | Quando recebe uma ISO europeia, explica que ela serve para gerar idiomas, em vez de só recusar. |
| [`xenon/dist/README.txt`](sms-xenon-source/xenon/dist/README.txt) | alterado (+11 linhas) | Nova seção "Languages". |
| [`xenon/dist/sms_env.txt`](sms-xenon-source/xenon/dist/sms_env.txt) | alterado (+4 linhas) | Linha comentada `SMS_LANGUAGE=pt`. |
| [`xenon/tools/xpackage`](sms-xenon-source/xenon/tools/xpackage) | alterado | Inclui `sms_language.py` no pacote distribuído. |
| [`xenon/tools/xstubs.py`](sms-xenon-source/xenon/tools/xstubs.py) | alterado (correção) | Funciona com o `compile_commands.json` do CMake do Ubuntu 22.04, que não tem o campo `output`, e avisa claramente quando falta a compilação Linux. Sem isso, o `xstage` parava no meio e o Visual Studio recebia código não convertido (erros C2059 em `J3DJoint.hpp`). |

| [`decomp/.gitattributes`](sms-xenon-source/decomp/.gitattributes) | alterado (1 linha) | `* text=auto` → `* text=auto eol=lf`. Sem isso, clonar no Windows deixava os fontes da descompilação com CRLF e o primeiro patch falhava (`gesso.cpp ... hunk FAILED`). Agora clonar no Ubuntu, clonar no Windows ou baixar o ZIP dão todos arquivos com LF. |

Nenhum arquivo C/C++ do jogo ou do port foi alterado: o suporte a idiomas usa o sistema de pacotes
que o port já tinha (`platform/misc/language.cpp`, `platform/dvd/dvd.cpp`).

## Arquivos novos na raiz do repositório

| Arquivo | Para que serve |
|---|---|
| `README.md`, `README.pt-BR.md` | Passo a passo completo e créditos, em inglês e em português. |
| `CHANGES.md`, `ALTERACOES.md` | Esta lista, em inglês e em português. |
| `LICENSE` | Licença das minhas adições (MIT), com referência às licenças originais. |
| `.gitignore` | Impede que ISOs, dados do jogo e arquivos compilados entrem no repositório. |
| `.gitattributes` | Guarda os arquivos byte a byte, para os scripts do Linux não quebrarem. |

## Ajuste técnico no primeiro commit

O código original foi importado exatamente como estava no `.7z`, com uma diferença que não muda o
conteúdo de nenhum arquivo: os 168 scripts que começam com `#!` (como `build.sh` e
`xenon/tools/xstage`) foram marcados como **executáveis** no Git. Extrair o `.7z` no Windows perde
essa permissão, e sem ela o build falha no Linux com "Permission denied".

Verificação: dos 2729 arquivos, 2718 estão guardados idênticos byte a byte ao `.7z`. Os outros 11
(`build.cmd`, `clean.cmd`, `run.cmd` e 8 arquivos `JSG*` em `decomp/libs/JSystem`) têm o mesmo
conteúdo, mas o Git os guarda com fim de linha LF, por causa dos `.gitattributes` do próprio projeto
original (`*.cmd text eol=crlf` e `* text=auto` em `decomp/`). Quando clonados, os `.cmd` voltam a ter
CRLF.

## Como o `sms_language.py` funciona

A ISO europeia guarda cada idioma em arquivos separados (`game_6_sp.szs`, `message_sp.bmg`, ...), e a
tradução PT-BR coloca o português no lugar do espanhol (`_sp`). O script:

1. Copia os diálogos de cada fase para `scene/<fase>.szs/map/message.bmg`.
2. Copia as mensagens do sistema (`cmn2d_<idioma>.szs`) para dentro de `common.szs/2d/`.
3. Copia a fonte do disco (com á, ã, ç, ê...) para `nintendo.szs`.
4. Reconstrói `game_6`, `guide`, `select`, `endsave`, `subtitle` e `option.szs`, juntando o arquivo
   americano com o do idioma. O sistema de pacotes do port só substitui arquivos dentro de um `.szs`,
   não acrescenta novos, e as versões PAL têm arquivos extras (como as letras de "¡BIEN!" e "BOA!").
5. Detecta sozinho se o slot espanhol tem português (ISO BR) e gera `pt` em vez de `es`.

O reconstrutor de arquivos RARC foi validado devolvendo **byte a byte** todos os 122 arquivos `.szs`
da ISO americana e os 147 da europeia.
