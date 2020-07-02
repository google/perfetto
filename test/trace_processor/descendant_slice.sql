SELECT slice.name AS currentSliceName, descendant.name AS descendantSliceName
FROM slice LEFT JOIN descendant_slice(slice.id) AS descendant
ORDER BY slice.ts ASC, descendant.ts ASC, slice.name ASC, descendant.name ASC;
