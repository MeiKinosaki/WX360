# Onix360

**Windows Xbox 360 Emulator** — emulador experimental do Xbox 360 para Windows x64, com foco em computadores modestos.

> **Versão em desenvolvimento:** v0.7b. O projeto ainda não executa jogos comerciais.
>
> **Marcos v0.7b validados em 24/11/2026:** testes de branches PowerPC `BCLR`/`BCCTR`, incluindo variantes com link, condições CR, destinos LR/CTR e semântica de CTR/LR passaram na CI #549 e #550. Isso valida apenas os testes do interpretador; não comprova boot de homebrew ou XEX comercial, assinatura RSA ou compatibilidade com jogos.

## Objetivos

- Windows 10/11 de 64 bits.
- x86-64 com SSE2; AVX/AVX2 não são requisitos.
- Meta de otimização para computadores com até 4 GB de RAM; isso não garante que jogos completos rodem nessa capacidade.
- Arquitetura modular para CPU Xenon, memória, GPU Xenos, áudio e kernel.

## Interface e relógio em tempo real

- O dashboard Win32 mantém o tema escuro azul/ciano e agora exibe no cabeçalho a hora local do Windows em formato 24 horas (`HH:MM:SS`) e a data (`DD/MM/AAAA`).
- A interface atualiza o relógio com um timer Win32 de 1 segundo e consulta `GetLocalTime`; não depende de internet nem de sincronização externa.
- A formatação está isolada em `include/onix360/ui/clock_display.hpp` e possui teste determinístico em `tests/clock_display_tests.cpp`.
- O relógio não altera a temporização do hardware emulado: é apenas um relógio de interface.

## Estado de desenvolvimento e limites

- O núcleo tem implementações experimentais de CPU PowerPC, MMU/memória, kernel/heap, parser XEX, leitores ISO/STFS/SVOD, framebuffer/comandos Xenos, áudio, controles e save states.
- Os dez slots de save state armazenam snapshots do subconjunto atualmente serializado; não representam ainda um estado integral de todos os dispositivos.
- A MMU hashed page table é um modelo PowerPC experimental, não uma implementação completa da MMU específica do Xenon.
- O renderer Direct3D 11 é inicial; Vulkan, Direct3D 12 e OpenGL não devem ser considerados backends nativos completos.
- O pipeline XEX, imports/exports, segurança/assinatura, GPU Xenos, XMA, kernel/XAM e compatibilidade ainda têm lacunas substanciais.
- Não declarar execução de XEX real, boot de jogos, build ou testes aprovados sem evidência de execução correspondente. Esta etapa não executa XEX nem jogos reais.

## CPU Xenon: incrementos recentes em validação

- O interpretador recebeu operações PowerPC de controle do FPSCR: `MCRFS`, `MTFSB0`, `MTFSB1` e `MTFSFI`.
- O subconjunto VMX recebeu comparações vetoriais por igualdade e maior que, para elementos de 8, 16 e 32 bits, com variantes assinadas e não assinadas.
- Foram adicionados testes sintéticos para essas instruções e corrigidos vetores de regressão de FPU/exceções.
- Esses incrementos estão submetidos à CI. Até a conclusão da execução, não são declarados como validados; a versão permanece v0.7b.

## Build e testes

Configure com CMake 3.20+ e execute o conjunto CTest. A CI deve validar o build/testes; a existência de alvos no CMake não significa que tenham sido executados ou aprovados.
