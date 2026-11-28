# Onix360

**Windows Xbox 360 Emulator** — emulador experimental do Xbox 360 para Windows x64, com foco em computadores modestos.

> **Versão em desenvolvimento:** v1.0.1-preview (prévia experimental; ainda não é uma versão estável). O projeto ainda não executa jogos comerciais.
>
> **Marcos v0.7b validados em 24/11/2026:** testes de branches PowerPC `BCLR`/`BCCTR`, incluindo variantes com link, condições CR, destinos LR/CTR e semântica de CTR/LR passaram na CI #549 e #550. Isso valida apenas os testes do interpretador; não comprova boot de homebrew ou XEX comercial, assinatura RSA ou compatibilidade com jogos.


## Marco 1.0.1 Preview

- Identificador de versão do projeto atualizado para `1.0.0` no CMake e `1.0.1-preview` na documentação.
- Este marco é uma prévia de desenvolvimento, não uma declaração de estabilidade ou compatibilidade com jogos.
- A promoção para 1.0.0 estável depende de CI verde em Windows e Linux, pacote portátil Windows gerado, regressões corrigidas e documentação das limitações remanescentes.

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
- Esses incrementos estão submetidos à CI. Até a conclusão da execução, não são declarados como validados; a versão permanece 1.0.1-preview.

## VMX: estado de condição das comparações

- Comparações vetoriais de inteiros e ponto flutuante atualizam CR6 quando o bit Rc está ativo.
- CR6 distingue vetor totalmente verdadeiro, totalmente falso e resultado misto.
- Testes sintéticos cobrem os três estados; a execução permanece sujeita à CI.

## VMX: operações de splat adicionadas

- `VSPLTB`, `VSPLTH` e `VSPLTW`: selecionam um elemento de VA e o replicam nos 128 bits de VD.
- `VSPLTISB`, `VSPLTISH` e `VSPLTISW`: estendem o imediato assinado de 5 bits e o replicam por todos os elementos do vetor.
- Testes sintéticos cobrem seleção de byte/halfword/word e extensão de imediato negativo.
- O suporte continua parcial: isso não equivale a implementar toda a ISA VMX nem comprova execução de jogos.

## Armazenamento de arquivos de convidado e save slots

- O sistema de arquivos de convidado agora cria arquivos dentro do sandbox, enumera entradas imediatas de diretórios e da raiz, renomeia arquivos e remove arquivos sem handles abertos.
- Caminhos continuam normalizados e protegidos contra traversal; nenhuma operação permite acesso arbitrário ao sistema de arquivos do host.
- Os dez slots agora expõem enumeração separada de slots ocupados e disponíveis e usam a extensão `.onix360`.
- O snapshot continua parcial: CPU e RAM são serializadas; GPU, áudio, dispositivos, scheduler e estado integral do kernel ainda não estão incluídos.
- Os novos testes foram enviados à CI e aguardam validação.

## Build e testes

Configure com CMake 3.20+ e execute o conjunto CTest. A CI deve validar o build/testes; a existência de alvos no CMake não significa que tenham sido executados ou aprovados.

## CPU PowerPC: instruções adicionadas nesta rodada

- `MCRXR` transfere SO/OV/CA do XER para um campo CR e limpa esses três bits no XER.
- `EXTSW` estende o sinal do word de 32 bits para 64 bits.
- `POPCNTB` conta bits por byte; `POPCNTW` e `POPCNTD` contam bits em word e doubleword.
- `ISEL` seleciona um registrador ou zero conforme um bit do CR.
- Testes sintéticos foram adicionados para os resultados, seleção de CR e atualização do XER. A CI desta revisão ainda precisa executar; não declarar esses testes aprovados antes do resultado.

## Cobertura adicional do registrador de condição PowerPC

- `RLWIMI` e `RLWNM`: rotação de word com inserção/máscara.
- `MCRF`: cópia de campos do Condition Register.
- Operações lógicas CR: `CRAND`, `CRANDC`, `CREQV`, `CRNAND`, `CRNOR`, `CROR`, `CRORC` e `CRXOR`.
- Testes sintéticos foram adicionados; a execução na CI desta revisão ainda precisa ser confirmada.

## Versão 1.0.1 Preview — sequência incremental

- Atualização sequencial de 1.0.0-preview para 1.0.1-preview.
- Inclui os incrementos recentes do interpretador PowerPC: ISEL, MCRXR, EXTSW, POPCNTB/W/D, RLWIMI, RLWNM, MCRF e operações lógicas do CR.
- A numeração identifica um marco de desenvolvimento, não uma versão estável. A validação de CI e as limitações descritas neste README continuam obrigatórias.
