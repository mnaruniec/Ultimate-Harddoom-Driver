Dostarczony Makefile jest w formacie takim jak Adlerdeva, tzn. w przypadku braku nagłówków jądra w /lib/modules należy podmienić wartość zmiennej KDIR na ścieżkę do źródeł kernela.
Rozwiązanie jest asynchroniczne i używa bloku BATCH.
Do obsługi wait używam listy aktywnych oczekiwań z kolejką procesów w każdym węźle.
Wszelkie alokacje są podzielone na strony, blok BATCH używa bufora wielkości jednej strony.
Wskaźniki do stron bufora są trzymane w liście.
Bufory do zliczania referencji przy mapowaniu do pamięci urządzenia wykorzystują już istniejący mechanizm zliczeń odwołań do struktury file.
Mapowania do pamięci urządzenia są trzymane na liście, nowe mapowanie jest dodawane na początek pierwszego obszaru, w którym wystarczy miejsca.
Na liście trzymane są również opisy tablic w katalogu stron, aby uniknąć dużej alokacji pełnej tablicy wpisów.
