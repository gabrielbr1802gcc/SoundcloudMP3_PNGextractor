# SoundcloudMP3_PNGextractor

Extrai a capa (thumbnail) embutida em um arquivo MP3 via ID3v2 (suporta ID3v2.2 `PIC` e ID3v2.3/2.4 `APIC`), sem dependências externas.

Motivação: alguns MP3 baixados (ex.: SoundCloud/downloaders) têm a capa “escondida” dentro da tag e o Windows/Android nem sempre mostra fácil.

## Como usar

### Modo interativo (bom no Code::Blocks)
- Rode o programa (Run)
- Cole o caminho do MP3 e aperte Enter
- (Opcional) informe a saída ou aperte Enter para padrão

### Linha de comando
- `thumb_extrac "C:\\Musicas\\teste.mp3"`
- `thumb_extrac "C:\\Musicas\\teste.mp3" "D:\\Capas\\"`
- Debug: `thumb_extrac --debug "C:\\Musicas\\teste.mp3"`

## Saída padrão
- Se não informar saída, salva ao lado do MP3 como `NOME_DO_MP3_cover.jpg/.png/...` conforme o conteúdo.
