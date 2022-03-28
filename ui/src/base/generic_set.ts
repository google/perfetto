// ES6 Set does not allow to reasonably store compound objects; this class
// rectifies the problem by implementing generic set on top of Map and an
// injective function from objects of generic type to strings.
export class GenericSet<T> {
  interner: (t: T) => string;

  // Passed function should be injective (as in never having the same output for
  // two different inputs).
  constructor(interner: (t: T) => string) {
    this.interner = interner;
  }

  backingMap = new Map<string, T>();

  has(column: T): boolean {
    return this.backingMap.has(this.interner(column));
  }

  add(column: T) {
    this.backingMap.set(this.interner(column), column);
  }

  delete(column: T) {
    this.backingMap.delete(this.interner(column));
  }

  values(): Iterable<T> {
    return this.backingMap.values();
  }
}
