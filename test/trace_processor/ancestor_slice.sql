SELECT slice.name AS currentSliceName, ancestor.name AS ancestorSliceName
FROM slice LEFT JOIN ancestor_slice(slice.id) AS ancestor
ORDER BY slice.ts ASC, ancestor.ts ASC, slice.name ASC, ancestor.name ASC;
