# Gerenciador de Memória Virtual (MMU)

Implementação em C do projeto, adaptado para memória física de **128 frames**, políticas de substituição **FIFO** e **LRU**  e busca paralela na TLB utilizando `pthreads`.

## Compilação

No terminal, dentro do diretório do projeto, execute:

```bash
make
./vm <arquivo_enderecos> <algoritmo_pagina> <algoritmo_tlb>
